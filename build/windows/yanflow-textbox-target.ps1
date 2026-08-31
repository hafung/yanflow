param(
    [Parameter(Mandatory = $true)][string]$ReadyFile,
    [Parameter(Mandatory = $true)][string]$ResultFile
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$expected = -join @([char]0x6ee8, [char]0x6d77, [char]0x65b0, [char]0x533a, [char]0x6709, [char]0x623f)
$form = [Windows.Forms.Form]::new()
$form.Text = "YanFlow isolated editable target"
$form.StartPosition = "CenterScreen"
$form.Size = [Drawing.Size]::new(680, 180)
$textbox = [Windows.Forms.TextBox]::new()
$textbox.Multiline = $true
$textbox.Dock = "Fill"
$textbox.Font = [Drawing.Font]::new("Microsoft YaHei UI", 16)
$textbox.Text = "YANFLOW-E2E:"
$textbox.SelectionStart = $textbox.TextLength
$form.Controls.Add($textbox)
$timer = [Windows.Forms.Timer]::new()
$timer.Interval = 50
$timer.Add_Tick({
    if ($textbox.Text.Contains($expected)) {
        [IO.File]::WriteAllText($ResultFile, $textbox.Text, [Text.UTF8Encoding]::new($false))
        $timer.Stop()
        $form.Close()
    }
})
$form.Add_Shown({
    $textbox.Focus()
    [IO.File]::WriteAllText($ReadyFile, [string]$PID)
    $timer.Start()
})
[void]$form.ShowDialog()
$timer.Dispose()
$textbox.Dispose()
$form.Dispose()
