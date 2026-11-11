Add-Type -AssemblyName System.Drawing
foreach ($p in $args) {
    try {
        $img = New-Object System.Drawing.Bitmap $p
        $rect = New-Object System.Drawing.Rectangle 0, 0, $img.Width, $img.Height
        $rgb = $img.Clone($rect, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
        $rgb.Save("$p.ref.bmp", [System.Drawing.Imaging.ImageFormat]::Bmp)
        $rgb.Dispose()
        $img.Dispose()
        "OK $p"
    } catch {
        "ERROR $p $($_.Exception.Message)"
    }
}
