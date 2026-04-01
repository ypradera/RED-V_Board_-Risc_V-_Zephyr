using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.IO.Ports;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Media3D;
using System.Windows.Shapes;
using System.Windows.Threading;

namespace ShoeVisualizer;

public partial class MainWindow : Window
{
    private SerialPort? _serial;
    private bool _connected;

    // IMU data
    private double _qi, _qj, _qk, _qr;
    private double _gx, _gy, _gz, _ax, _ay, _az, _vx, _vy, _vz;
    private string _tempStr = "—", _luxStr = "—";

    // Euler angles (degrees)
    private double _pitch, _roll, _yaw;

    // Gait analysis
    private int _stepCount;
    private long _lastHeelStrikeMs;
    private double _lastAccelZ;
    private bool _inSwing; // true = foot in air
    private readonly List<long> _strideTimes = new();
    private string _gaitPhase = "—";
    private const double HEEL_STRIKE_THRESHOLD = 2.0; // m/s² spike

    // Strip chart data (rolling window)
    private const int CHART_POINTS = 200;
    private readonly double[] _pitchHistory = new double[CHART_POINTS];
    private readonly double[] _rollHistory = new double[CHART_POINTS];
    private readonly double[] _accelZHistory = new double[CHART_POINTS];
    private int _chartIdx;

    // Stats
    private int _packetCount, _frameCount;
    private readonly Stopwatch _fpsTimer = new();
    private readonly Stopwatch _gaitTimer = new();

    // 3D
    private QuaternionRotation3D _quatRotation = null!;
    private Point _lastMouse;
    private bool _isDragging;
    private double _camDist = 25, _camTheta = 0.8, _camPhi = 0.5;

    public MainWindow()
    {
        InitializeComponent();
        PopulatePorts();
        BuildScene();
        UpdateCamera();

        var timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(16) };
        timer.Tick += RenderTick;
        timer.Start();
        _fpsTimer.Start();
        _gaitTimer.Start();

        // Mouse orbit
        Viewport.MouseLeftButtonDown += (s, e) => { _isDragging = true; _lastMouse = e.GetPosition(Viewport); Viewport.CaptureMouse(); };
        Viewport.MouseLeftButtonUp += (s, e) => { _isDragging = false; Viewport.ReleaseMouseCapture(); };
        Viewport.MouseMove += (s, e) =>
        {
            if (!_isDragging) return;
            var p = e.GetPosition(Viewport);
            _camTheta += (p.X - _lastMouse.X) * 0.01;
            _camPhi -= (p.Y - _lastMouse.Y) * 0.01;
            _camPhi = Math.Clamp(_camPhi, 0.1, Math.PI - 0.1);
            _lastMouse = p;
            UpdateCamera();
        };
        Viewport.MouseWheel += (s, e) =>
        {
            _camDist -= e.Delta * 0.02;
            _camDist = Math.Clamp(_camDist, 2, 100);
            UpdateCamera();
        };
    }

    private void UpdateCamera()
    {
        double x = _camDist * Math.Sin(_camPhi) * Math.Cos(_camTheta);
        double y = _camDist * Math.Cos(_camPhi);
        double z = _camDist * Math.Sin(_camPhi) * Math.Sin(_camTheta);
        var cam = (PerspectiveCamera)Viewport.Camera;
        cam.Position = new Point3D(x, y, z);
        cam.LookDirection = new Vector3D(-x, -y, -z);
    }

    // ---- Quaternion to Euler ----

    private void QuatToEuler(double qi, double qj, double qk, double qr)
    {
        // Roll (X) — inversion/eversion
        double sinr = 2 * (qr * qi + qj * qk);
        double cosr = 1 - 2 * (qi * qi + qj * qj);
        _roll = Math.Atan2(sinr, cosr) * 180 / Math.PI;

        // Pitch (Y) — dorsiflexion/plantarflexion
        double sinp = 2 * (qr * qj - qk * qi);
        _pitch = Math.Abs(sinp) >= 1
            ? Math.CopySign(90, sinp)
            : Math.Asin(sinp) * 180 / Math.PI;

        // Yaw (Z) — foot rotation
        double siny = 2 * (qr * qk + qi * qj);
        double cosy = 1 - 2 * (qj * qj + qk * qk);
        _yaw = Math.Atan2(siny, cosy) * 180 / Math.PI;
    }

    // ---- Gait Event Detection ----

    private void DetectGaitEvents()
    {
        double accelMag = Math.Sqrt(_ax * _ax + _ay * _ay + _az * _az);
        long now = _gaitTimer.ElapsedMilliseconds;

        // Heel strike: sharp acceleration spike after swing phase
        if (_inSwing && accelMag > HEEL_STRIKE_THRESHOLD)
        {
            _inSwing = false;
            _gaitPhase = "STANCE (Heel Strike)";
            _stepCount++;

            if (_lastHeelStrikeMs > 0)
            {
                long stride = now - _lastHeelStrikeMs;
                _strideTimes.Add(stride);
                if (_strideTimes.Count > 20) _strideTimes.RemoveAt(0);
            }
            _lastHeelStrikeMs = now;
        }
        // Toe-off: gyroscope spike during stance → start of swing
        else if (!_inSwing && Math.Abs(_gy) > 3.0) // >3 rad/s rotation
        {
            _inSwing = true;
            _gaitPhase = "SWING (Toe-Off)";
        }
        // Midswing: foot in air, moderate rotation
        else if (_inSwing && accelMag < HEEL_STRIKE_THRESHOLD)
        {
            _gaitPhase = "SWING";
        }
        // Midstance: foot on ground, low activity
        else if (!_inSwing && accelMag < 0.5)
        {
            _gaitPhase = "STANCE (Midstance)";
        }

        _lastAccelZ = _az;
    }

    // ---- Serial ----

    private void PopulatePorts()
    {
        PortCombo.Items.Clear();
        foreach (var p in SerialPort.GetPortNames()) PortCombo.Items.Add(p);
        if (PortCombo.Items.Count > 0) PortCombo.SelectedIndex = PortCombo.Items.Count - 1;
    }

    private void ConnectBtn_Click(object sender, RoutedEventArgs e)
    {
        if (_connected) { Disconnect(); return; }
        if (PortCombo.SelectedItem == null) { PopulatePorts(); return; }
        try
        {
            _serial = new SerialPort(PortCombo.SelectedItem.ToString()!, 115200);
            _serial.DataReceived += (s, ev) =>
            {
                try { while (_serial!.BytesToRead > 0) { var l = _serial.ReadLine()?.Trim(); if (l != null) { _packetCount++; Parse(l); } } } catch { }
            };
            _serial.Open();
            _connected = true;
            ConnectBtn.Content = "Disconnect";
            StatusText.Text = $"Connected: {_serial.PortName}";
            StatusText.Foreground = new SolidColorBrush(Color.FromRgb(22, 199, 154));
        }
        catch (Exception ex) { StatusText.Text = ex.Message; }
    }

    private void Disconnect()
    {
        try { _serial?.Close(); } catch { }
        _serial = null; _connected = false;
        ConnectBtn.Content = "Connect";
        StatusText.Text = "Disconnected";
        StatusText.Foreground = new SolidColorBrush(Color.FromRgb(233, 69, 96));
    }

    private void Parse(string line)
    {
        var s = StringSplitOptions.RemoveEmptyEntries;
        if (line.StartsWith("[imu] Q:"))
        {
            var p = line[8..].Trim().Split(' ', s);
            if (p.Length >= 4)
            {
                _qi = P(p[0]) / 1e4; _qj = P(p[1]) / 1e4;
                _qk = P(p[2]) / 1e4; _qr = P(p[3]) / 1e4;
                QuatToEuler(_qi, _qj, _qk, _qr);
            }
        }
        else if (line.StartsWith("[imu] G:")) { var p = line[8..].Trim().Split(' ', s); if (p.Length >= 3) { _gx = P(p[0]) / 1e2; _gy = P(p[1]) / 1e2; _gz = P(p[2]) / 1e2; } }
        else if (line.StartsWith("[imu] A:"))
        {
            var p = line[8..].Trim().Split(' ', s);
            if (p.Length >= 3)
            {
                _ax = P(p[0]) / 1e2; _ay = P(p[1]) / 1e2; _az = P(p[2]) / 1e2;
                DetectGaitEvents();
            }
        }
        else if (line.StartsWith("[imu] V:")) { var p = line[8..].Trim().Split(' ', s); if (p.Length >= 3) { _vx = P(p[0]) / 1e2; _vy = P(p[1]) / 1e2; _vz = P(p[2]) / 1e2; } }
        else if (line.StartsWith("[shtc3]")) _tempStr = line[7..].Trim();
        else if (line.StartsWith("[opt4048]")) _luxStr = line[9..].Trim();
    }

    static double P(string s) => int.TryParse(s, NumberStyles.Integer, CultureInfo.InvariantCulture, out int v) ? v : 0;

    // ---- Strip Charts ----

    private void UpdateChart(Canvas canvas, double[] data, int idx, Color color, double minVal, double maxVal)
    {
        canvas.Children.Clear();
        double w = canvas.ActualWidth, h = canvas.ActualHeight;
        if (w < 10 || h < 10) return;

        // Zero line
        double zeroY = h * (maxVal / (maxVal - minVal));
        var zeroLine = new Line { X1 = 0, Y1 = zeroY, X2 = w, Y2 = zeroY,
            Stroke = new SolidColorBrush(Color.FromArgb(40, 255, 255, 255)), StrokeThickness = 1 };
        canvas.Children.Add(zeroLine);

        // Data polyline
        var poly = new Polyline { Stroke = new SolidColorBrush(color), StrokeThickness = 1.5 };
        double step = w / CHART_POINTS;
        for (int i = 0; i < CHART_POINTS; i++)
        {
            int di = (idx + i + 1) % CHART_POINTS;
            double val = Math.Clamp(data[di], minVal, maxVal);
            double x = i * step;
            double y = h - ((val - minVal) / (maxVal - minVal)) * h;
            poly.Points.Add(new Point(x, y));
        }
        canvas.Children.Add(poly);

        // Current value text
        double current = data[idx];
        var txt = new TextBlock
        {
            Text = $"{current:F1}",
            Foreground = new SolidColorBrush(color),
            FontSize = 11, FontFamily = new FontFamily("Consolas")
        };
        Canvas.SetRight(txt, 5);
        Canvas.SetTop(txt, 2);
        canvas.Children.Add(txt);
    }

    // ---- 3D Scene ----

    private void BuildScene()
    {
        var gridGroup = new Model3DGroup();
        for (int i = -5; i <= 5; i++)
        {
            Box(gridGroup, i, -0.5, 0, 0.02, 0.02, 10, Color.FromArgb(50, 100, 180, 255));
            Box(gridGroup, 0, -0.5, i, 10, 0.02, 0.02, Color.FromArgb(50, 100, 180, 255));
        }
        GridVisual.Content = gridGroup;

        var shoeGroup = new Model3DGroup();
        string objPath = System.IO.Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "Models", "sneakers.obj");
        bool loaded = false;

        if (File.Exists(objPath))
        {
            try
            {
                var mesh = LoadObj(objPath, singleShoe: true);
                if (mesh != null)
                {
                    var mat = new DiffuseMaterial(new SolidColorBrush(Color.FromRgb(200, 40, 40)));
                    var spec = new SpecularMaterial(Brushes.White, 30);
                    var mg = new MaterialGroup();
                    mg.Children.Add(mat); mg.Children.Add(spec);
                    shoeGroup.Children.Add(new GeometryModel3D(mesh, mg) { BackMaterial = mat });
                    loaded = true;
                }
            }
            catch { }
        }
        if (!loaded) BuildFallbackShoe(shoeGroup);

        _quatRotation = new QuaternionRotation3D(new Quaternion(0, 0, 0, 1));
        var xform = new Transform3DGroup();
        xform.Children.Add(new ScaleTransform3D(6, 6, 6));
        xform.Children.Add(new RotateTransform3D(_quatRotation));
        ShoeVisual.Content = shoeGroup;
        ShoeVisual.Transform = xform;
    }

    static MeshGeometry3D? LoadObj(string path, bool singleShoe = true)
    {
        var verts = new List<Point3D>();
        var faces = new List<int[]>();
        foreach (var raw in File.ReadLines(path))
        {
            var line = raw.Trim();
            if (line.StartsWith("v "))
            {
                var p = line[2..].Split(' ', StringSplitOptions.RemoveEmptyEntries);
                if (p.Length >= 3) verts.Add(new Point3D(D(p[0]), D(p[1]), D(p[2])));
            }
            else if (line.StartsWith("f "))
            {
                var p = line[2..].Split(' ', StringSplitOptions.RemoveEmptyEntries);
                if (p.Length >= 3)
                {
                    var fv = new int[p.Length];
                    for (int i = 0; i < p.Length; i++) fv[i] = FI(p[i]);
                    faces.Add(fv);
                }
            }
        }
        if (verts.Count == 0) return null;

        double midX = 0;
        if (singleShoe)
        {
            double mn = double.MaxValue, mx = double.MinValue;
            foreach (var v in verts) { if (v.X < mn) mn = v.X; if (v.X > mx) mx = v.X; }
            midX = (mn + mx) / 2;
        }

        var nv = new List<Point3D>();
        var map = new int[verts.Count];
        for (int i = 0; i < map.Length; i++) map[i] = -1;

        for (int i = 0; i < verts.Count; i++)
        {
            if (!singleShoe || verts[i].X >= midX)
            {
                map[i] = nv.Count;
                nv.Add(verts[i]);
            }
        }

        // Center the single shoe
        if (singleShoe && nv.Count > 0)
        {
            double cx = 0, cy = 0, cz = 0;
            foreach (var v in nv) { cx += v.X; cy += v.Y; cz += v.Z; }
            cx /= nv.Count; cy /= nv.Count; cz /= nv.Count;
            for (int i = 0; i < nv.Count; i++)
                nv[i] = new Point3D(nv[i].X - cx, nv[i].Y - cy, nv[i].Z - cz);
        }

        var m = new MeshGeometry3D();
        foreach (var v in nv) m.Positions.Add(v);
        foreach (var face in faces)
        {
            bool ok = true;
            foreach (var vi in face) if (vi < 0 || vi >= verts.Count || map[vi] < 0) { ok = false; break; }
            if (!ok) continue;
            int v0 = map[face[0]];
            for (int i = 1; i < face.Length - 1; i++)
            { m.TriangleIndices.Add(v0); m.TriangleIndices.Add(map[face[i]]); m.TriangleIndices.Add(map[face[i + 1]]); }
        }
        return m.Positions.Count > 0 ? m : null;
    }

    static double D(string s) => double.Parse(s, CultureInfo.InvariantCulture);
    static int FI(string s) { var sl = s.IndexOf('/'); return int.Parse(sl >= 0 ? s[..sl] : s, CultureInfo.InvariantCulture) - 1; }

    static void BuildFallbackShoe(Model3DGroup g)
    {
        Box(g, 0, -0.15, 0.3, 1, 0.3, 3, Color.FromRgb(240, 240, 240));
        Box(g, 0, 0.2, 0.2, 0.9, 0.5, 2.6, Color.FromRgb(200, 40, 40));
        Box(g, 0, 0.3, 1.5, 0.85, 0.6, 0.4, Color.FromRgb(200, 40, 40));
        Box(g, 0, 0.55, 0, 0.5, 0.05, 1.2, Color.FromRgb(30, 30, 30));
        Box(g, 0.46, 0.2, 0.2, 0.02, 0.3, 1.5, Color.FromRgb(30, 30, 30));
        Box(g, -0.46, 0.2, 0.2, 0.02, 0.3, 1.5, Color.FromRgb(30, 30, 30));
    }

    static void Box(Model3DGroup g, double cx, double cy, double cz,
                    double sx, double sy, double sz, Color c)
    {
        var m = new MeshGeometry3D();
        double hx = sx / 2, hy = sy / 2, hz = sz / 2;
        m.Positions.Add(new Point3D(cx-hx,cy-hy,cz-hz)); m.Positions.Add(new Point3D(cx+hx,cy-hy,cz-hz));
        m.Positions.Add(new Point3D(cx+hx,cy+hy,cz-hz)); m.Positions.Add(new Point3D(cx-hx,cy+hy,cz-hz));
        m.Positions.Add(new Point3D(cx-hx,cy-hy,cz+hz)); m.Positions.Add(new Point3D(cx+hx,cy-hy,cz+hz));
        m.Positions.Add(new Point3D(cx+hx,cy+hy,cz+hz)); m.Positions.Add(new Point3D(cx-hx,cy+hy,cz+hz));
        foreach (var i in new[] { 0,2,1, 0,3,2, 4,5,6, 4,6,7, 0,1,5, 0,5,4,
                                   2,3,7, 2,7,6, 0,4,7, 0,7,3, 1,2,6, 1,6,5 })
            m.TriangleIndices.Add(i);
        var mat = new DiffuseMaterial(new SolidColorBrush(c));
        g.Children.Add(new GeometryModel3D(m, mat) { BackMaterial = mat });
    }

    // ---- Render ----

    private void RenderTick(object? sender, EventArgs e)
    {
        _frameCount++;

        // Update 3D rotation
        if (_qr != 0 || _qi != 0 || _qj != 0 || _qk != 0)
            _quatRotation.Quaternion = new Quaternion(_qi, _qj, _qk, _qr);

        // Update strip chart data
        _pitchHistory[_chartIdx] = _pitch;
        _rollHistory[_chartIdx] = _roll;
        _accelZHistory[_chartIdx] = Math.Sqrt(_ax * _ax + _ay * _ay + _az * _az);

        // Foot angles
        PitchText.Text = $"Pitch: {_pitch:F1}°";
        RollText.Text = $"Roll: {_roll:F1}°";
        YawText.Text = $"Yaw: {_yaw:F1}°";

        // Gait info
        GaitPhaseText.Text = $"Phase: {_gaitPhase}";
        GaitPhaseText.Foreground = new SolidColorBrush(
            _inSwing ? Color.FromRgb(255, 200, 50) : Color.FromRgb(50, 255, 150));
        StepCountText.Text = $"Steps: {_stepCount}";

        // Cadence
        if (_strideTimes.Count >= 2)
        {
            double avgStride = 0;
            foreach (var t in _strideTimes) avgStride += t;
            avgStride /= _strideTimes.Count;
            double cadence = 60000.0 / avgStride;
            CadenceText.Text = $"Cadence: {cadence:F0} steps/min";
            StrideTimeText.Text = $"Stride: {avgStride:F0} ms";
        }

        // Raw IMU
        QuatText.Text = $"Q: {_qi:F3} {_qj:F3} {_qk:F3} {_qr:F3}";
        GyroText.Text = $"G: {_gx:F2} {_gy:F2} {_gz:F2}";
        AccelText.Text = $"A: {_ax:F2} {_ay:F2} {_az:F2}";
        GravText.Text = $"V: {_vx:F2} {_vy:F2} {_vz:F2}";
        TempText.Text = _tempStr; LuxText.Text = _luxStr;
        PacketText.Text = $"Packets: {_packetCount}";

        // Draw strip charts
        UpdateChart(PitchChart, _pitchHistory, _chartIdx, Color.FromRgb(255, 215, 0), -90, 90);
        UpdateChart(RollChart, _rollHistory, _chartIdx, Color.FromRgb(0, 255, 127), -90, 90);
        UpdateChart(AccelZChart, _accelZHistory, _chartIdx, Color.FromRgb(255, 107, 107), 0, 10);

        _chartIdx = (_chartIdx + 1) % CHART_POINTS;

        if (_fpsTimer.ElapsedMilliseconds >= 1000)
        { FpsText.Text = $"FPS: {_frameCount}"; _frameCount = 0; _fpsTimer.Restart(); }
    }

    protected override void OnClosed(EventArgs e) { Disconnect(); base.OnClosed(e); }
}
