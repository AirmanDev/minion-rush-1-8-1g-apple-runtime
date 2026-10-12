param(
    [Parameter(Mandatory = $true)]
    [string]$Kit
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

$script:kitPath = [System.IO.Path]::GetFullPath($Kit)
$script:pythonPath = Join-Path $script:kitPath 'python\python.exe'
$script:builderPath = Join-Path $script:kitPath 'runtime\tools\windows_native_package.py'
$script:worker = $null
$script:standardOutTask = $null
$script:standardErrorTask = $null
$script:scratchPath = $null
$script:cancelRequested = $false

function Quote-ProcessArgument([string]$value) {
    $escaped = $value -replace '(\\*)"', '$1$1\"'
    $escaped = $escaped -replace '(\\+)$', '$1$1'
    return '"' + $escaped + '"'
}

function Add-Control($control) {
    $form.Controls.Add($control)
    return $control
}

function Add-Message([string]$message) {
    if ([string]::IsNullOrWhiteSpace($message)) { return }
    $log.AppendText($message + [Environment]::NewLine)
    if ($log.TextLength -gt 64000) {
        $log.Text = $log.Text.Substring($log.TextLength - 32000)
    }
    $log.SelectionStart = $log.TextLength
    $log.ScrollToCaret()
    if ($message -match '^[123]/3:') {
        $status.Text = $message
    }
}

function Set-Busy([bool]$busy) {
    $zipButton.Enabled = -not $busy
    $dropArea.Enabled = -not $busy
    $outputButton.Enabled = -not $busy
    $buildButton.Enabled = -not $busy
    $cancelButton.Enabled = $busy
    $progress.Visible = $busy
    if ($busy) { $progress.Style = 'Marquee' }
}

function Select-Zip {
    $dialog = New-Object System.Windows.Forms.OpenFileDialog
    $dialog.Title = 'Select your original 1.8.1g ZIP'
    $dialog.Filter = 'ZIP archives (*.zip)|*.zip'
    $dialog.CheckFileExists = $true
    $dialog.Multiselect = $false
    try {
        if ($dialog.ShowDialog($form) -eq [System.Windows.Forms.DialogResult]::OK) {
            $zipField.Text = $dialog.FileName
        }
    } finally {
        $dialog.Dispose()
    }
}

function Set-ZipFromDrop([string[]]$paths) {
    if ($paths.Count -ne 1 -or
        -not $paths[0].EndsWith('.zip', [System.StringComparison]::OrdinalIgnoreCase) -or
        -not [System.IO.File]::Exists($paths[0])) {
        [System.Windows.Forms.MessageBox]::Show(
            $form, 'Drop one existing ZIP archive.', 'Unsupported selection',
            'OK', 'Warning') | Out-Null
        return
    }
    $zipField.Text = $paths[0]
}

function Select-Output {
    $dialog = New-Object System.Windows.Forms.SaveFileDialog
    $dialog.Title = 'Choose where to save the unsigned IPA'
    $dialog.Filter = 'iOS IPA archives (*.ipa)|*.ipa'
    $dialog.DefaultExt = 'ipa'
    $dialog.AddExtension = $true
    $dialog.OverwritePrompt = $false
    $dialog.FileName = 'MinionRush-unsigned.ipa'
    $downloads = Join-Path $env:USERPROFILE 'Downloads'
    if (Test-Path -LiteralPath $downloads -PathType Container) {
        $dialog.InitialDirectory = $downloads
    }
    try {
        if ($dialog.ShowDialog($form) -eq [System.Windows.Forms.DialogResult]::OK) {
            $outputField.Text = $dialog.FileName
        }
    } finally {
        $dialog.Dispose()
    }
}

function Remove-Scratch {
    if ($null -ne $script:scratchPath) {
        $path = $script:scratchPath
        $script:scratchPath = $null
        if (Test-Path -LiteralPath $path) {
            try {
                Remove-Item -LiteralPath $path -Recurse -Force -ErrorAction Stop
            } catch {
                Add-Message ('Could not remove private temporary files: ' + $path)
            }
        }
    }
}

function Stop-Worker {
    if ($null -eq $script:worker -or $script:worker.HasExited) { return }
    $script:cancelRequested = $true
    $status.Text = 'Stopping the build...'
    $stopper = Join-Path $env:SystemRoot 'System32\taskkill.exe'
    $kill = New-Object System.Diagnostics.ProcessStartInfo
    $kill.FileName = $stopper
    $kill.Arguments = '/PID ' + $script:worker.Id + ' /T /F'
    $kill.UseShellExecute = $false
    $kill.CreateNoWindow = $true
    $kill.RedirectStandardOutput = $true
    $kill.RedirectStandardError = $true
    $terminator = [System.Diagnostics.Process]::Start($kill)
    try {
        $null = $terminator.StandardOutput.ReadToEnd()
        $null = $terminator.StandardError.ReadToEnd()
        $terminator.WaitForExit()
    } finally {
        $terminator.Dispose()
    }
}

function Start-Build {
    $source = $zipField.Text.Trim()
    $destination = $outputField.Text.Trim()
    if (-not [System.IO.File]::Exists($source) -or
        -not $source.EndsWith('.zip', [System.StringComparison]::OrdinalIgnoreCase)) {
        $status.Text = 'Select an existing ZIP file.'
        return
    }
    if ([string]::IsNullOrWhiteSpace($destination) -or
        -not $destination.EndsWith('.ipa', [System.StringComparison]::OrdinalIgnoreCase)) {
        $status.Text = 'Choose an .ipa destination first.'
        return
    }
    try {
        $destination = [System.IO.Path]::GetFullPath($destination)
        if ([System.IO.File]::Exists($destination) -or
            [System.IO.Directory]::Exists($destination)) {
            $status.Text = 'That output already exists. Choose a new filename.'
            return
        }
        if (-not [System.IO.Directory]::Exists(
            [System.IO.Path]::GetDirectoryName($destination))) {
            $status.Text = 'The destination folder does not exist.'
            return
        }
        if (-not [System.IO.File]::Exists($script:pythonPath) -or
            -not [System.IO.File]::Exists($script:builderPath)) {
            $status.Text = 'The portable Windows kit is incomplete.'
            return
        }
        $catalog = Join-Path (Split-Path -Parent $script:kitPath) 'MinionRush-Private-Languages\hu.xlf'
        if (-not [System.IO.File]::Exists($catalog)) {
            $status.Text = 'Private hu.xlf is required beside the kit.'
            Add-Message 'Expected: MinionRush-Private-Languages\hu.xlf next to the kit folder.'
            return
        }

        $script:scratchPath = Join-Path ([System.IO.Path]::GetTempPath()) (
            'mr-windows-gui-' + [guid]::NewGuid().ToString('N'))
        [System.IO.Directory]::CreateDirectory($script:scratchPath) | Out-Null
        $info = New-Object System.Diagnostics.ProcessStartInfo
        $info.FileName = $script:pythonPath
        $arguments = @($script:builderPath, $source, '--kit', $script:kitPath,
                       '--output', $destination)
        $info.Arguments = ($arguments | ForEach-Object { Quote-ProcessArgument $_ }) -join ' '
        $info.WorkingDirectory = $script:kitPath
        $info.UseShellExecute = $false
        $info.CreateNoWindow = $true
        $info.RedirectStandardOutput = $true
        $info.RedirectStandardError = $true
        $info.EnvironmentVariables['TMP'] = $script:scratchPath
        $info.EnvironmentVariables['TEMP'] = $script:scratchPath
        $script:worker = New-Object System.Diagnostics.Process
        $script:worker.StartInfo = $info
        $script:cancelRequested = $false
        if (-not $script:worker.Start()) {
            throw 'Could not start the portable Python runtime.'
        }
        $script:standardOutTask = $script:worker.StandardOutput.ReadLineAsync()
        $script:standardErrorTask = $script:worker.StandardError.ReadLineAsync()
        $log.Clear()
        $status.Text = 'Validating game files and preparing the unsigned IPA...'
        Set-Busy $true
        $timer.Start()
    } catch {
        if ($null -ne $script:worker) {
            if (-not $script:worker.HasExited) { Stop-Worker }
            $script:worker.Dispose()
            $script:worker = $null
        }
        Remove-Scratch
        Set-Busy $false
        $status.Text = 'The build could not start.'
        Add-Message $_.Exception.Message
    }
}

$form = New-Object System.Windows.Forms.Form
$form.Text = 'Unofficial 1.8.1g IPA Builder'
$form.ClientSize = [System.Drawing.Size]::new(760, 560)
$form.MinimumSize = [System.Drawing.Size]::new(700, 590)
$form.StartPosition = 'CenterScreen'
$form.Font = [System.Drawing.Font]::new('Segoe UI', 10)
$form.AutoScaleMode = 'Dpi'
$form.KeyPreview = $true

$heading = Add-Control (New-Object System.Windows.Forms.Label)
$heading.Text = 'Create an unsigned iPhone app'
$heading.Font = [System.Drawing.Font]::new('Segoe UI', 17, [System.Drawing.FontStyle]::Bold)
$heading.Location = [System.Drawing.Point]::new(24, 17)
$heading.Size = [System.Drawing.Size]::new(700, 38)

$subheading = Add-Control (New-Object System.Windows.Forms.Label)
$subheading.Text = 'Your original ZIP stays on this computer. No Apple account is needed here.'
$subheading.Location = [System.Drawing.Point]::new(26, 58)
$subheading.Size = [System.Drawing.Size]::new(710, 27)

$sourceLabel = Add-Control (New-Object System.Windows.Forms.Label)
$sourceLabel.Text = '1. Original game ZIP'
$sourceLabel.Location = [System.Drawing.Point]::new(25, 105)
$sourceLabel.Size = [System.Drawing.Size]::new(460, 28)

$dropArea = Add-Control (New-Object System.Windows.Forms.Label)
$dropArea.Text = 'Drop one .zip here or click to browse'
$dropArea.TextAlign = 'MiddleCenter'
$dropArea.BorderStyle = 'FixedSingle'
$dropArea.AllowDrop = $true
$dropArea.Cursor = 'Hand'
$dropArea.Location = [System.Drawing.Point]::new(25, 135)
$dropArea.Size = [System.Drawing.Size]::new(710, 75)
$dropArea.Anchor = 'Top, Left, Right'
$dropArea.Add_Click({ Select-Zip })
$dropArea.Add_DragEnter({
    param($sender, $eventArgs)
    $eventArgs.Effect = 'None'
    if ($eventArgs.Data.GetDataPresent([System.Windows.Forms.DataFormats]::FileDrop)) {
        $files = @($eventArgs.Data.GetData([System.Windows.Forms.DataFormats]::FileDrop))
        if ($files.Count -eq 1 -and
            $files[0].EndsWith('.zip', [System.StringComparison]::OrdinalIgnoreCase)) {
            $eventArgs.Effect = 'Copy'
        }
    }
})
$dropArea.Add_DragDrop({
    param($sender, $eventArgs)
    Set-ZipFromDrop @($eventArgs.Data.GetData([System.Windows.Forms.DataFormats]::FileDrop))
})

$zipField = Add-Control (New-Object System.Windows.Forms.TextBox)
$zipField.Location = [System.Drawing.Point]::new(25, 216)
$zipField.Size = [System.Drawing.Size]::new(595, 31)
$zipField.Anchor = 'Top, Left, Right'
$zipField.ReadOnly = $true

$zipButton = Add-Control (New-Object System.Windows.Forms.Button)
$zipButton.Text = 'Browse'
$zipButton.Location = [System.Drawing.Point]::new(631, 213)
$zipButton.Size = [System.Drawing.Size]::new(104, 33)
$zipButton.Anchor = 'Top, Right'
$zipButton.Add_Click({ Select-Zip })

$outputLabel = Add-Control (New-Object System.Windows.Forms.Label)
$outputLabel.Text = '2. Save the IPA as'
$outputLabel.Location = [System.Drawing.Point]::new(25, 275)
$outputLabel.Size = [System.Drawing.Size]::new(440, 28)

$outputField = Add-Control (New-Object System.Windows.Forms.TextBox)
$outputField.Location = [System.Drawing.Point]::new(25, 310)
$outputField.Size = [System.Drawing.Size]::new(595, 31)
$outputField.Anchor = 'Top, Left, Right'
$outputField.ReadOnly = $true

$outputButton = Add-Control (New-Object System.Windows.Forms.Button)
$outputButton.Text = 'Choose'
$outputButton.Location = [System.Drawing.Point]::new(631, 307)
$outputButton.Size = [System.Drawing.Size]::new(104, 33)
$outputButton.Anchor = 'Top, Right'
$outputButton.Add_Click({ Select-Output })

$status = Add-Control (New-Object System.Windows.Forms.Label)
$status.Text = 'Select the source ZIP and an output location.'
$status.Location = [System.Drawing.Point]::new(25, 365)
$status.Size = [System.Drawing.Size]::new(705, 28)
$status.Anchor = 'Top, Left, Right'
$status.AutoEllipsis = $true

$progress = Add-Control (New-Object System.Windows.Forms.ProgressBar)
$progress.Location = [System.Drawing.Point]::new(25, 396)
$progress.Size = [System.Drawing.Size]::new(710, 6)
$progress.Anchor = 'Top, Left, Right'
$progress.Visible = $false

$log = Add-Control (New-Object System.Windows.Forms.TextBox)
$log.Location = [System.Drawing.Point]::new(25, 412)
$log.Size = [System.Drawing.Size]::new(710, 65)
$log.Anchor = 'Top, Bottom, Left, Right'
$log.Multiline = $true
$log.ReadOnly = $true
$log.ScrollBars = 'Vertical'
$log.Font = [System.Drawing.Font]::new('Consolas', 9)

$signing = Add-Control (New-Object System.Windows.Forms.Label)
$signing.Text = 'Signing and installation happen separately. Not affiliated with Gameloft.'
$signing.Location = [System.Drawing.Point]::new(25, 488)
$signing.Size = [System.Drawing.Size]::new(530, 38)
$signing.Anchor = 'Bottom, Left, Right'

$buildButton = Add-Control (New-Object System.Windows.Forms.Button)
$buildButton.Text = 'Create IPA'
$buildButton.Location = [System.Drawing.Point]::new(615, 511)
$buildButton.Size = [System.Drawing.Size]::new(120, 36)
$buildButton.Anchor = 'Bottom, Right'
$buildButton.Add_Click({ Start-Build })

$cancelButton = Add-Control (New-Object System.Windows.Forms.Button)
$cancelButton.Text = 'Cancel'
$cancelButton.Location = [System.Drawing.Point]::new(489, 511)
$cancelButton.Size = [System.Drawing.Size]::new(116, 36)
$cancelButton.Anchor = 'Bottom, Right'
$cancelButton.Enabled = $false
$cancelButton.Add_Click({ Stop-Worker })

$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 100
$timer.Add_Tick({
    if ($null -eq $script:worker) { return }
    for ($i = 0; $i -lt 200 -and $null -ne $script:standardOutTask -and
        $script:standardOutTask.IsCompleted; $i++) {
        $line = $script:standardOutTask.Result
        if ($null -eq $line) {
            $script:standardOutTask = $null
        } else {
            Add-Message $line
            $script:standardOutTask = $script:worker.StandardOutput.ReadLineAsync()
        }
    }
    for ($i = 0; $i -lt 200 -and $null -ne $script:standardErrorTask -and
        $script:standardErrorTask.IsCompleted; $i++) {
        $line = $script:standardErrorTask.Result
        if ($null -eq $line) {
            $script:standardErrorTask = $null
        } else {
            Add-Message $line
            $script:standardErrorTask = $script:worker.StandardError.ReadLineAsync()
        }
    }
    if ($script:worker.HasExited -and
        $null -eq $script:standardOutTask -and $null -eq $script:standardErrorTask) {
        $code = $script:worker.ExitCode
        $script:worker.Dispose()
        $script:worker = $null
        $timer.Stop()
        Remove-Scratch
        Set-Busy $false
        if ($script:cancelRequested) {
            $status.Text = 'Build cancelled.'
        } elseif ($code -eq 0 -and [System.IO.File]::Exists($outputField.Text)) {
            $status.Text = 'Unsigned IPA created successfully. Sign it before installation.'
            [System.Windows.Forms.MessageBox]::Show(
                $form, 'The unsigned IPA was saved to the selected location.' +
                [Environment]::NewLine + $outputField.Text +
                [Environment]::NewLine + 'Use your own signing tool before installation.',
                'Build completed', 'OK', 'Information') | Out-Null
        } else {
            $status.Text = 'Build failed. Check the details above.'
        }
    }
})

$form.Add_KeyDown({
    param($sender, $eventArgs)
    if ($eventArgs.Control -and $eventArgs.KeyCode -eq 'O' -and
        $null -eq $script:worker) {
        Select-Zip
        $eventArgs.SuppressKeyPress = $true
    }
})
$form.Add_FormClosing({
    param($sender, $eventArgs)
    if ($null -ne $script:worker -and -not $script:worker.HasExited) {
        $answer = [System.Windows.Forms.MessageBox]::Show(
            $form, 'Stop the build and close the application?', 'Build in progress',
            'YesNo', 'Warning')
        if ($answer -ne [System.Windows.Forms.DialogResult]::Yes) {
            $eventArgs.Cancel = $true
            return
        }
        Stop-Worker
        if (-not $script:worker.WaitForExit(10000)) {
            $eventArgs.Cancel = $true
            $status.Text = 'The worker is still running. Close was cancelled.'
            return
        }
    }
    $timer.Stop()
    if ($null -ne $script:worker) {
        $script:worker.Dispose()
        $script:worker = $null
    }
    Remove-Scratch
})

try {
    [System.Windows.Forms.Application]::Run($form)
} finally {
    $timer.Dispose()
    $form.Dispose()
}
