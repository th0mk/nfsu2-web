# runtime/switch/make-icon.ps1 -- draws icon.jpg (256x256, the size the Switch's menus want) for the NRO.
# The game's own logo (cut out of a menu capture: its light pixels, over a night street) with a Switch touch: the red and blue
# of the Joy-Con at the two sides.  Usage: make-icon.ps1 <capture.png with the title logo at 1280x720>
param([string]$Capture = (Join-Path $PSScriptRoot '..\..\out\shots\gl_last.png'))
Add-Type -AssemblyName System.Drawing
$W = 256
$src = New-Object System.Drawing.Bitmap $Capture

# the logo: x 205..545, y 36..88 of the menu capture; its brightness becomes its opacity, so the menu's dark panel drops out
$lx = 205; $ly = 34; $lw = 342; $lh = 56
$logo = New-Object System.Drawing.Bitmap $lw, $lh, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
for ($y = 0; $y -lt $lh; $y++) { for ($x = 0; $x -lt $lw; $x++) {
  $p = $src.GetPixel($lx + $x, $ly + $y)
  $lum = [int](0.3 * $p.R + 0.59 * $p.G + 0.11 * $p.B)
  $a = [Math]::Min(255, [Math]::Max(0, ($lum - 55) * 3))
  $logo.SetPixel($x, $y, [System.Drawing.Color]::FromArgb($a, [Math]::Min(255, $p.R + 30), [Math]::Min(255, $p.G + 30), [Math]::Min(255, $p.B + 30)))
} }

$bmp = New-Object System.Drawing.Bitmap $W, $W
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode = 'AntiAlias'; $g.InterpolationMode = 'HighQualityBicubic'; $g.PixelOffsetMode = 'HighQuality'

# night street: dark blue down to a dark purple
$bg = New-Object System.Drawing.Drawing2D.LinearGradientBrush ([System.Drawing.Point]::new(0, 0)), ([System.Drawing.Point]::new(0, $W)), ([System.Drawing.Color]::FromArgb(12, 16, 36)), ([System.Drawing.Color]::FromArgb(46, 12, 52))
$g.FillRectangle($bg, 0, 0, $W, $W)

# the street: converging edges, a dashed centre line, and neon streaks of speed
$road = New-Object System.Drawing.Drawing2D.GraphicsPath
$road.AddPolygon(@([System.Drawing.PointF]::new(128, 120), [System.Drawing.PointF]::new(-60, 256), [System.Drawing.PointF]::new(316, 256)))
$g.FillPath((New-Object System.Drawing.Drawing2D.LinearGradientBrush ([System.Drawing.Point]::new(0, 120)), ([System.Drawing.Point]::new(0, 256)), ([System.Drawing.Color]::FromArgb(0, 0, 0, 0)), ([System.Drawing.Color]::FromArgb(210, 6, 6, 14))), $road)
$dash = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(230, 255, 140, 0)), 3
$dash.DashStyle = 'Dash'
$g.DrawLine($dash, 128, 132, 128, 256)
foreach ($st in @(@(0, 150, 'Cyan', 100, 3), @(0, 176, 'Magenta', 110, 4), @(40, 205, 'Cyan', 120, 2), @(0, 122, 'Magenta', 70, 2), @(70, 232, 'Cyan', 80, 3), @(0, 98, 'Cyan', 60, 2))) {
  $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb($st[3], [System.Drawing.Color]::FromName($st[2]))), $st[4]
  $g.DrawLine($pen, $st[0], $st[1] + 40, $st[0] + 220, $st[1] - 40)
}

# the logo, wide, with a soft glow behind it
$tw = 228; $th = [int]($lh * $tw / $lw); $tx = (($W - $tw) / 2); $ty = 74
$glow = New-Object System.Drawing.Drawing2D.GraphicsPath
$glow.AddEllipse(8, $ty - 26, 240, $th + 52)
$gb = New-Object System.Drawing.Drawing2D.PathGradientBrush $glow
$gb.CenterColor = [System.Drawing.Color]::FromArgb(90, 0, 170, 255); $gb.SurroundColors = @([System.Drawing.Color]::FromArgb(0, 0, 170, 255))
$g.FillPath($gb, $glow)
$g.DrawImage($logo, [float]$tx, [float]$ty, [float]$tw, [float]$th)

# the Joy-Con touch: neon red on the left, neon blue on the right (rounded bars), and a thin frame
$red = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 255, 60, 80))
$blue = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 0, 195, 227))
$g.FillRectangle($red, 0, 0, 14, $W); $g.FillRectangle($blue, ($W - 14), 0, 14, $W)
$g.FillEllipse((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 30, 30, 36))), 3, 118, 8, 8)
$g.FillEllipse((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 30, 30, 36))), ($W - 11), 118, 8, 8)

$small = New-Object System.Drawing.Font 'Arial', 15, ([System.Drawing.FontStyle]::Bold), ([System.Drawing.GraphicsUnit]::Pixel)
$sf = New-Object System.Drawing.StringFormat; $sf.Alignment = 'Center'
$g.DrawString('ONLINE', $small, (New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(235, 255, 200, 60))), 128.0, 226.0, $sf)

$enc = [System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object { $_.MimeType -eq 'image/jpeg' }
$ep = New-Object System.Drawing.Imaging.EncoderParameters 1
$ep.Param[0] = New-Object System.Drawing.Imaging.EncoderParameter ([System.Drawing.Imaging.Encoder]::Quality), ([long]92)
$out = Join-Path $PSScriptRoot 'icon.jpg'
$bmp.Save($out, $enc, $ep)
$g.Dispose(); $bmp.Dispose(); $src.Dispose()
"wrote $out ($((Get-Item $out).Length) bytes)"
