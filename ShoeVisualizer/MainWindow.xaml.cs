using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.IO.Ports;
using System.Linq;
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

    // IMU data (written on background thread, read on UI thread)
    // Benign race: worst case UI shows one stale frame
    private double _qi, _qj, _qk, _qr;
    private double _gx, _gy, _gz, _ax, _ay, _az, _vx, _vy, _vz;
    private double _pitch, _roll, _yaw;
    private string _tempStr = "—", _luxStr = "—";
    private string _gaitPhase = "—";
    private bool _inSwing;
    private int _stepCount;
    private int _packetCount;

    // Gait analysis (background thread only, synchronized via lock)
    private long _lastHeelStrikeMs;
    private readonly object _strideLock = new();
    private readonly Queue<long> _strideTimes = new();
    private readonly Stopwatch _gaitTimer = new();

    // Strip chart data
    private const int ChartPoints = 200;
    private readonly double[] _pitchHistory = new double[ChartPoints];
    private readonly double[] _rollHistory = new double[ChartPoints];
    private readonly double[] _accelMagHistory = new double[ChartPoints];
    private int _chartIdx;

    // Chart visuals (created once, updated in-place)
    private Polyline _pitchLine = null!, _rollLine = null!, _accelLine = null!;
    private Line _pitchZero = null!, _rollZero = null!, _accelZero = null!;
    private TextBlock _pitchVal = null!, _rollVal = null!, _accelVal = null!;

    // Stats
    private int _frameCount;
    private readonly Stopwatch _fpsTimer = new();

    // 3D
    private QuaternionRotation3D _quatRotation = null!;
    private Point _lastMouse;
    private bool _isDragging;
    private double _camDist = 25, _camTheta = 0.8, _camPhi = 0.5;

    // Thresholds
    private const double HeelStrikeThreshold = 2.0;
    private const double ToeOffGyroThreshold = 3.0;
    private const double MidstanceAccelThreshold = 0.5;
    private const int MaxStrideHistory = 20;
    private const double MouseOrbitSensitivity = 0.01;
    private const double MouseZoomSensitivity = 0.02;

    // Cached frozen brushes
    private static readonly SolidColorBrush SwingBrush = Freeze(Colors.Gold);
    private static readonly SolidColorBrush StanceBrush = Freeze(Color.FromRgb(50, 255, 150));
    private static readonly SolidColorBrush DisconnectedBrush = Freeze(Color.FromRgb(233, 69, 96));
    private static readonly SolidColorBrush ConnectedBrush = Freeze(Color.FromRgb(22, 199, 154));

    private static SolidColorBrush Freeze(Color c) { var b = new SolidColorBrush(c); b.Freeze(); return b; }

    public MainWindow()
    {
        InitializeComponent();
        PopulatePorts();
        BuildScene();
        SetupCharts();
        UpdateCamera();

        var timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(16) };
        timer.Tick += RenderTick;
        timer.Start();
        _fpsTimer.Start();
        _gaitTimer.Start();

        Viewport.MouseLeftButtonDown += (s, e) => { _isDragging = true; _lastMouse = e.GetPosition(Viewport); Viewport.CaptureMouse(); };
        Viewport.MouseLeftButtonUp += (s, e) => { _isDragging = false; Viewport.ReleaseMouseCapture(); };
        Viewport.MouseMove += (s, e) =>
        {
            if (!_isDragging) return;
            var p = e.GetPosition(Viewport);
            _camTheta += (p.X - _lastMouse.X) * MouseOrbitSensitivity;
            _camPhi -= (p.Y - _lastMouse.Y) * MouseOrbitSensitivity;
            _camPhi = Math.Clamp(_camPhi, 0.1, Math.PI - 0.1);
            _lastMouse = p;
            UpdateCamera();
        };
        Viewport.MouseWheel += (s, e) =>
        {
            _camDist -= e.Delta * MouseZoomSensitivity;
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

    private void QuatToEuler(double qi, double qj, double qk, double qr)
    {
        double sinr = 2 * (qr * qi + qj * qk);
        double cosr = 1 - 2 * (qi * qi + qj * qj);
        _roll = Math.Atan2(sinr, cosr) * (180.0 / Math.PI);

        double sinp = 2 * (qr * qj - qk * qi);
        _pitch = Math.Abs(sinp) >= 1 ? Math.CopySign(90, sinp) : Math.Asin(sinp) * (180.0 / Math.PI);

        double siny = 2 * (qr * qk + qi * qj);
        double cosy = 1 - 2 * (qj * qj + qk * qk);
        _yaw = Math.Atan2(siny, cosy) * (180.0 / Math.PI);
    }

    private void DetectGaitEvents()
    {
        double accelMag = Math.Sqrt(_ax * _ax + _ay * _ay + _az * _az);
        long now = _gaitTimer.ElapsedMilliseconds;

        if (_inSwing && accelMag > HeelStrikeThreshold)
        {
            _inSwing = false;
            _gaitPhase = "STANCE (Heel Strike)";
            _stepCount++;
            if (_lastHeelStrikeMs > 0)
            {
                lock (_strideLock)
                {
                    _strideTimes.Enqueue(now - _lastHeelStrikeMs);
                    while (_strideTimes.Count > MaxStrideHistory) _strideTimes.Dequeue();
                }
            }
            _lastHeelStrikeMs = now;
        }
        else if (!_inSwing && Math.Abs(_gy) > ToeOffGyroThreshold)
        {
            _inSwing = true;
            _gaitPhase = "SWING (Toe-Off)";
        }
        else if (_inSwing && accelMag < HeelStrikeThreshold)
        {
            _gaitPhase = "SWING";
        }
        else if (!_inSwing && accelMag < MidstanceAccelThreshold)
        {
            _gaitPhase = "STANCE (Midstance)";
        }
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
        if (_serial != null) { Disconnect(); return; }
        if (PortCombo.SelectedItem == null) { PopulatePorts(); return; }
        try
        {
            _serial = new SerialPort(PortCombo.SelectedItem.ToString()!, 115200);
            _serial.DataReceived += OnSerialData;
            _serial.Open();
            ConnectBtn.Content = "Disconnect";
            StatusText.Text = $"Connected: {_serial.PortName}";
            StatusText.Foreground = ConnectedBrush;
        }
        catch (Exception ex) { StatusText.Text = ex.Message; }
    }

    private void Disconnect()
    {
        try { _serial?.Close(); } catch { }
        _serial = null;
        ConnectBtn.Content = "Connect";
        StatusText.Text = "Disconnected";
        StatusText.Foreground = DisconnectedBrush;
    }

    private void OnSerialData(object sender, SerialDataReceivedEventArgs e)
    {
        try
        {
            while (_serial is { IsOpen: true, BytesToRead: > 0 })
            {
                var line = _serial.ReadLine()?.Trim();
                if (line != null) { _packetCount++; Parse(line); }
            }
        }
        catch { /* serial disconnect or partial read */ }
    }

    private void Parse(string line)
    {
        const StringSplitOptions s = StringSplitOptions.RemoveEmptyEntries;
        if (line.StartsWith("[imu] Q:"))
        {
            var p = line[8..].Trim().Split(' ', s);
            if (p.Length >= 4)
            {
                _qi = ParseInt(p[0]) / 1e4; _qj = ParseInt(p[1]) / 1e4;
                _qk = ParseInt(p[2]) / 1e4; _qr = ParseInt(p[3]) / 1e4;
                QuatToEuler(_qi, _qj, _qk, _qr);
            }
        }
        else if (line.StartsWith("[imu] G:"))
        {
            var p = line[8..].Trim().Split(' ', s);
            if (p.Length >= 3) { _gx = ParseInt(p[0]) / 1e2; _gy = ParseInt(p[1]) / 1e2; _gz = ParseInt(p[2]) / 1e2; }
        }
        else if (line.StartsWith("[imu] A:"))
        {
            var p = line[8..].Trim().Split(' ', s);
            if (p.Length >= 3) { _ax = ParseInt(p[0]) / 1e2; _ay = ParseInt(p[1]) / 1e2; _az = ParseInt(p[2]) / 1e2; DetectGaitEvents(); }
        }
        else if (line.StartsWith("[imu] V:"))
        {
            var p = line[8..].Trim().Split(' ', s);
            if (p.Length >= 3) { _vx = ParseInt(p[0]) / 1e2; _vy = ParseInt(p[1]) / 1e2; _vz = ParseInt(p[2]) / 1e2; }
        }
        else if (line.StartsWith("[shtc3]")) _tempStr = line[7..].Trim();
        else if (line.StartsWith("[opt4048]")) _luxStr = line[9..].Trim();
    }

    private static double ParseInt(string s) =>
        int.TryParse(s, NumberStyles.Integer, CultureInfo.InvariantCulture, out int v) ? v : 0;

    // ---- Strip Charts (created once, updated in-place) ----

    private void SetupCharts()
    {
        SetupOneChart(PitchChart, Color.FromRgb(255, 215, 0), out _pitchLine, out _pitchZero, out _pitchVal);
        SetupOneChart(RollChart, Color.FromRgb(0, 255, 127), out _rollLine, out _rollZero, out _rollVal);
        SetupOneChart(AccelZChart, Color.FromRgb(255, 107, 107), out _accelLine, out _accelZero, out _accelVal);
    }

    private static void SetupOneChart(Canvas canvas, Color color,
        out Polyline line, out Line zero, out TextBlock val)
    {
        var brush = new SolidColorBrush(color); brush.Freeze();
        var dimBrush = new SolidColorBrush(Color.FromArgb(40, 255, 255, 255)); dimBrush.Freeze();

        zero = new Line { Stroke = dimBrush, StrokeThickness = 1 };
        canvas.Children.Add(zero);

        line = new Polyline { Stroke = brush, StrokeThickness = 1.5 };
        canvas.Children.Add(line);

        val = new TextBlock { Foreground = brush, FontSize = 11, FontFamily = new FontFamily("Consolas") };
        Canvas.SetRight(val, 5);
        Canvas.SetTop(val, 2);
        canvas.Children.Add(val);
    }

    private void UpdateChart(Canvas canvas, Polyline line, Line zero, TextBlock val,
        double[] data, int idx, double minVal, double maxVal)
    {
        double w = canvas.ActualWidth, h = canvas.ActualHeight;
        if (w < 10 || h < 10) return;

        zero.X1 = 0; zero.X2 = w;
        zero.Y1 = zero.Y2 = h * (maxVal / (maxVal - minVal));

        var pts = new PointCollection(ChartPoints);
        double step = w / ChartPoints;
        for (int i = 0; i < ChartPoints; i++)
        {
            int di = (idx + i + 1) % ChartPoints;
            double v = Math.Clamp(data[di], minVal, maxVal);
            pts.Add(new Point(i * step, h - ((v - minVal) / (maxVal - minVal)) * h));
        }
        line.Points = pts;
        val.Text = $"{data[idx]:F1}";
    }

    // ---- 3D Scene ----

    private void BuildScene()
    {
        var gridGroup = new Model3DGroup();
        var gridColor = Color.FromArgb(50, 100, 180, 255);
        for (int i = -5; i <= 5; i++)
        {
            Box(gridGroup, i, -0.5, 0, 0.02, 0.02, 10, gridColor);
            Box(gridGroup, 0, -0.5, i, 10, 0.02, 0.02, gridColor);
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
                    var mg = new MaterialGroup();
                    mg.Children.Add(new DiffuseMaterial(new SolidColorBrush(Color.FromRgb(200, 40, 40))));
                    mg.Children.Add(new SpecularMaterial(Brushes.White, 30));
                    shoeGroup.Children.Add(new GeometryModel3D(mesh, mg)
                        { BackMaterial = new DiffuseMaterial(new SolidColorBrush(Color.FromRgb(200, 40, 40))) });
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

    private static MeshGeometry3D? LoadObj(string path, bool singleShoe = true)
    {
        var verts = new List<Point3D>();
        var faces = new List<int[]>();
        foreach (var raw in File.ReadLines(path))
        {
            var line = raw.Trim();
            if (line.StartsWith("v "))
            {
                var p = line[2..].Split(' ', StringSplitOptions.RemoveEmptyEntries);
                if (p.Length >= 3) verts.Add(new Point3D(ParseDouble(p[0]), ParseDouble(p[1]), ParseDouble(p[2])));
            }
            else if (line.StartsWith("f "))
            {
                var p = line[2..].Split(' ', StringSplitOptions.RemoveEmptyEntries);
                if (p.Length >= 3)
                {
                    var fv = new int[p.Length];
                    for (int i = 0; i < p.Length; i++) fv[i] = ParseFaceIndex(p[i]);
                    faces.Add(fv);
                }
            }
        }
        if (verts.Count == 0) return null;

        if (!singleShoe)
        {
            var m = new MeshGeometry3D();
            foreach (var v in verts) m.Positions.Add(v);
            foreach (var face in faces)
            {
                int v0 = face[0];
                for (int i = 1; i < face.Length - 1; i++)
                { m.TriangleIndices.Add(v0); m.TriangleIndices.Add(face[i]); m.TriangleIndices.Add(face[i + 1]); }
            }
            return m;
        }

        // Filter to one shoe (X >= midpoint) and center it
        double midX = (verts.Min(v => v.X) + verts.Max(v => v.X)) / 2;
        var filtered = new List<Point3D>();
        var vertMap = new int[verts.Count];
        Array.Fill(vertMap, -1);

        for (int i = 0; i < verts.Count; i++)
        {
            if (verts[i].X >= midX) { vertMap[i] = filtered.Count; filtered.Add(verts[i]); }
        }

        double cx = filtered.Average(v => v.X);
        double cy = filtered.Average(v => v.Y);
        double cz = filtered.Average(v => v.Z);
        for (int i = 0; i < filtered.Count; i++)
            filtered[i] = new Point3D(filtered[i].X - cx, filtered[i].Y - cy, filtered[i].Z - cz);

        var mesh = new MeshGeometry3D();
        foreach (var v in filtered) mesh.Positions.Add(v);
        foreach (var face in faces)
        {
            if (face.Any(vi => vi < 0 || vi >= verts.Count || vertMap[vi] < 0)) continue;
            int v0 = vertMap[face[0]];
            for (int i = 1; i < face.Length - 1; i++)
            { mesh.TriangleIndices.Add(v0); mesh.TriangleIndices.Add(vertMap[face[i]]); mesh.TriangleIndices.Add(vertMap[face[i + 1]]); }
        }
        return mesh.Positions.Count > 0 ? mesh : null;
    }

    private static double ParseDouble(string s) => double.Parse(s, CultureInfo.InvariantCulture);
    private static int ParseFaceIndex(string s)
    {
        var slash = s.IndexOf('/');
        return int.Parse(slash >= 0 ? s[..slash] : s, CultureInfo.InvariantCulture) - 1;
    }

    private static void BuildFallbackShoe(Model3DGroup g)
    {
        var red = Color.FromRgb(200, 40, 40);
        var black = Color.FromRgb(30, 30, 30);
        Box(g, 0, -0.15, 0.3, 1, 0.3, 3, Color.FromRgb(240, 240, 240));
        Box(g, 0, 0.2, 0.2, 0.9, 0.5, 2.6, red);
        Box(g, 0, 0.3, 1.5, 0.85, 0.6, 0.4, red);
        Box(g, 0, 0.55, 0, 0.5, 0.05, 1.2, black);
        Box(g, 0.46, 0.2, 0.2, 0.02, 0.3, 1.5, black);
        Box(g, -0.46, 0.2, 0.2, 0.02, 0.3, 1.5, black);
    }

    private static void Box(Model3DGroup g, double cx, double cy, double cz,
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

        if (_qr != 0 || _qi != 0 || _qj != 0 || _qk != 0)
            _quatRotation.Quaternion = new Quaternion(_qi, _qj, _qk, _qr);

        _pitchHistory[_chartIdx] = _pitch;
        _rollHistory[_chartIdx] = _roll;
        _accelMagHistory[_chartIdx] = Math.Sqrt(_ax * _ax + _ay * _ay + _az * _az);

        PitchText.Text = $"Pitch: {_pitch:F1}\u00b0";
        RollText.Text = $"Roll: {_roll:F1}\u00b0";
        YawText.Text = $"Yaw: {_yaw:F1}\u00b0";

        GaitPhaseText.Text = $"Phase: {_gaitPhase}";
        GaitPhaseText.Foreground = _inSwing ? SwingBrush : StanceBrush;
        StepCountText.Text = $"Steps: {_stepCount}";

        lock (_strideLock)
        {
            if (_strideTimes.Count >= 2)
            {
                double avg = _strideTimes.Average();
                CadenceText.Text = $"Cadence: {60000.0 / avg:F0} steps/min";
                StrideTimeText.Text = $"Stride: {avg:F0} ms";
            }
        }

        QuatText.Text = $"Q: {_qi:F3} {_qj:F3} {_qk:F3} {_qr:F3}";
        GyroText.Text = $"G: {_gx:F2} {_gy:F2} {_gz:F2}";
        AccelText.Text = $"A: {_ax:F2} {_ay:F2} {_az:F2}";
        GravText.Text = $"V: {_vx:F2} {_vy:F2} {_vz:F2}";
        TempText.Text = _tempStr;
        LuxText.Text = _luxStr;
        PacketText.Text = $"Packets: {_packetCount}";

        UpdateChart(PitchChart, _pitchLine, _pitchZero, _pitchVal, _pitchHistory, _chartIdx, -90, 90);
        UpdateChart(RollChart, _rollLine, _rollZero, _rollVal, _rollHistory, _chartIdx, -90, 90);
        UpdateChart(AccelZChart, _accelLine, _accelZero, _accelVal, _accelMagHistory, _chartIdx, 0, 10);
        _chartIdx = (_chartIdx + 1) % ChartPoints;

        if (_fpsTimer.ElapsedMilliseconds >= 1000)
        { FpsText.Text = $"FPS: {_frameCount}"; _frameCount = 0; _fpsTimer.Restart(); }
    }

    protected override void OnClosed(EventArgs e) { Disconnect(); base.OnClosed(e); }
}
