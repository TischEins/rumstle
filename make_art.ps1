# Rebuilds resource\*.png from the artist's exports. Run after any graphic changed:
#   powershell -ExecutionPolicy Bypass -File make_art.ps1
# Sources are 300-dpi exports (about 4x the nominal size), so everything is
# resampled to the size the plugin draws at, once for 1x and once for 2x.
param([string]$Gfx = 'D:\Projekte\TischEins\VST\Rumstle\GFX')
Add-Type -AssemblyName System.Drawing
$out = Join-Path $PSScriptRoot 'resource'

function New-Canvas($w, $h) {
    $b = New-Object System.Drawing.Bitmap($w, $h, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $c = [System.Drawing.Graphics]::FromImage($b)
    $c.InterpolationMode = 'HighQualityBicubic'; $c.PixelOffsetMode = 'HighQuality'
    $c.CompositingQuality = 'HighQuality'; $c.SmoothingMode = 'HighQuality'
    $b, $c
}

function Fit($src, $dst, $w, $h) {
    $i = [System.Drawing.Image]::FromFile((Join-Path $Gfx $src)); $b, $c = New-Canvas $w $h
    $a = New-Object System.Drawing.Imaging.ImageAttributes; $a.SetWrapMode('TileFlipXY')   # no dark fringe at the edges
    $c.DrawImage($i, (New-Object System.Drawing.Rectangle(0, 0, $w, $h)), 0, 0, $i.Width, $i.Height, 'Pixel', $a)
    $c.Dispose(); $b.Save((Join-Path $out $dst), [System.Drawing.Imaging.ImageFormat]::Png); $b.Dispose(); $i.Dispose()
}

# The knob is two layers so its lighting stays put: a fixed body and a pointer
# that turns on top of it.
# Pointer frames: the pointer graphic (at 12 o'clock) rotated in 1 degree
# steps from -135 to +135. 271 frames in a 16-column grid, because one tall strip
# would exceed Direct2D's 16384 px bitmap limit. kKnobFrames / kKnobCols in
# kastle_editor.cpp must match these two numbers.
$Frames = 271; $Cols = 16
function Knob($src, $dst, $size) {
    $rows = [math]::Ceiling($Frames / $Cols)
    $i = [System.Drawing.Image]::FromFile((Join-Path $Gfx $src)); $b, $c = New-Canvas ($size * $Cols) ($size * $rows)
    for ($f = 0; $f -lt $Frames; $f++) {
        $x = ($f % $Cols) * $size; $y = [math]::Floor($f / $Cols) * $size
        $c.ResetTransform(); $c.SetClip((New-Object System.Drawing.Rectangle($x, $y, $size, $size)))
        $c.TranslateTransform($x + $size / 2, $y + $size / 2); $c.RotateTransform(-135 + $f)
        $c.DrawImage($i, [single](-$size / 2), [single](-$size / 2), [single]$size, [single]$size)
    }
    $c.Dispose(); $b.Save((Join-Path $out $dst), [System.Drawing.Imaging.ImageFormat]::Png); $b.Dispose(); $i.Dispose()
}

Fit 'Klein\Interface760x650.png' 'plate.png' 760 650
Fit 'Gross\Interface1520x1300.png' 'plate_2x.png' 1520 1300
Fit  'Gross\regler_koerper.png' 'knob_body.png' 68 68;  Fit  'Gross\regler_koerper.png' 'knob_body_2x.png' 136 136
Fit  'Gross\regler_koerper.png' 'trim_body.png' 40 40;  Fit  'Gross\regler_koerper.png' 'trim_body_2x.png' 80 80
Knob 'Gross\regler_zeiger.png' 'knob.png' 68;  Knob 'Gross\regler_zeiger.png' 'knob_2x.png' 136
Knob 'Gross\regler_zeiger.png' 'trim.png' 40;  Knob 'Gross\regler_zeiger.png' 'trim_2x.png' 80
# switch and LEDs are drawn larger than their file names say: at 30x22 the
# OFF/ON lettering is unreadable
foreach ($st in 'off', 'on') {
    Fit "Gross\kippschalter_${st}44x60.png" "switch_$st.png" 80 59
    Fit "Gross\kippschalter_${st}44x60.png" "switch_${st}_2x.png" 160 118
    Fit "Gross\led_${st}_32x32.png" "led_$st.png" 24 24
    Fit "Gross\led_${st}_32x32.png" "led_${st}_2x.png" 48 48
}
Get-ChildItem $out -Filter *.png | Select-Object Name, Length
