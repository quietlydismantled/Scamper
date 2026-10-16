param([string]$Source, [string]$Destination)
# Builds a multi-size .ico: 256 px stored as PNG, smaller sizes as 32-bit DIBs (best Windows 7 support).
Add-Type -AssemblyName System.Drawing
$sizes = 16, 20, 24, 32, 40, 48, 64, 256
$art = [System.Drawing.Image]::FromFile($Source)
$images = foreach ($size in $sizes) {
    $bitmap = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bitmap)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
    $g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)
    $g.DrawImage($art, 0, 0, $size, $size)
    $g.Dispose()

    $stream = New-Object System.IO.MemoryStream
    if ($size -eq 256) {
        $bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
    } else {
        $w = New-Object System.IO.BinaryWriter $stream
        $w.Write([uint32]40); $w.Write([int32]$size); $w.Write([int32]($size * 2))
        $w.Write([uint16]1); $w.Write([uint16]32); $w.Write([uint32]0)
        $maskStride = [int]([math]::Ceiling($size / 32) * 4)
        $w.Write([uint32]($size * $size * 4 + $maskStride * $size))
        $w.Write([int32]0); $w.Write([int32]0); $w.Write([uint32]0); $w.Write([uint32]0)
        for ($y = $size - 1; $y -ge 0; $y--) {          # bottom-up BGRA
            for ($x = 0; $x -lt $size; $x++) {
                $c = $bitmap.GetPixel($x, $y)
                $w.Write([byte]$c.B); $w.Write([byte]$c.G); $w.Write([byte]$c.R); $w.Write([byte]$c.A)
            }
        }
        for ($y = $size - 1; $y -ge 0; $y--) {          # AND mask: 1 = transparent
            $row = New-Object byte[] $maskStride
            for ($x = 0; $x -lt $size; $x++) {
                if ($bitmap.GetPixel($x, $y).A -eq 0) { $row[$x -shr 3] = $row[$x -shr 3] -bor (0x80 -shr ($x -band 7)) }
            }
            $w.Write($row)
        }
        $w.Flush()
    }
    $bitmap.Dispose()
    [pscustomobject]@{ Size = $size; Bytes = $stream.ToArray() }
}
$art.Dispose()

$out = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter $out
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$images.Count)
$offset = 6 + 16 * $images.Count
foreach ($image in $images) {
    $dim = if ($image.Size -ge 256) { 0 } else { $image.Size }
    $w.Write([byte]$dim); $w.Write([byte]$dim); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([uint16]1); $w.Write([uint16]32)
    $w.Write([uint32]$image.Bytes.Length); $w.Write([uint32]$offset)
    $offset += $image.Bytes.Length
}
foreach ($image in $images) { $w.Write($image.Bytes) }
$w.Flush()
[IO.File]::WriteAllBytes($Destination, $out.ToArray())
"Wrote $Destination ($($out.Length) bytes, sizes: $($sizes -join ', '))"

