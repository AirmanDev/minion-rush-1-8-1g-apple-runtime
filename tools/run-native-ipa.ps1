$ErrorActionPreference = 'Stop'
$kit = Split-Path -Parent $MyInvocation.MyCommand.Path
$python = Join-Path $kit 'python\python.exe'
$builder = Join-Path $kit 'runtime\tools\windows_native_package.py'
if (-not (Test-Path -LiteralPath $python -PathType Leaf)) {
    Write-Host 'ERROR: Embedded Python is missing.'
    exit 1
}
if (-not (Test-Path -LiteralPath $builder -PathType Leaf)) {
    Write-Host 'ERROR: Windows native builder is missing.'
    exit 1
}
$privateZip = Join-Path (Split-Path -Parent $kit) 'original-1.8.1g-PRIVATE.zip'
if (-not (Test-Path -LiteralPath $privateZip -PathType Leaf)) {
    try {
        Add-Type -AssemblyName System.Windows.Forms
        $dialog = New-Object System.Windows.Forms.OpenFileDialog
        $dialog.Title = 'Select your own original Minion Rush 1.8.1g ZIP'
        $dialog.Filter = 'ZIP archives (*.zip)|*.zip'
        $dialog.CheckFileExists = $true
        $dialog.Multiselect = $false
        if ($dialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) {
            Write-Host 'No original game ZIP was selected.'
            exit 1
        }
        $privateZip = $dialog.FileName
    } catch {
        Write-Host ('Cannot select source ZIP: ' + $_.Exception.Message)
        exit 1
    }
}
$downloads = Join-Path $env:USERPROFILE 'Downloads'
if (-not (Test-Path -LiteralPath $downloads -PathType Container)) {
    Write-Host ('ERROR: Windows Downloads directory missing: ' + $downloads)
    exit 1
}
$output = Join-Path $downloads ('MinionRush-unsigned-' + [guid]::NewGuid().ToString('N') + '.ipa')
$report = Join-Path $kit 'windows-native-ipa-report.txt'
Write-Host 'Using a private, locally supplied 1.8.1g ZIP.'
Write-Host ('Unsigned IPA destination: ' + $output)
& $python $builder $privateZip --kit $kit --output $output
$code = $LASTEXITCODE
if ($code -ne 0) {
    'FAIL: Windows native IPA preparation returned exit code ' + $code |
        Out-File -FilePath $report -Encoding utf8
    Write-Host ('Native IPA builder returned error: ' + $code)
    exit $code
}
if (-not (Test-Path -LiteralPath $output -PathType Leaf)) {
    'FAIL: Windows native IPA output file is missing.' |
        Out-File -FilePath $report -Encoding utf8
    Write-Host 'ERROR: IPA output file was not created.'
    exit 1
}
$size = (Get-Item -LiteralPath $output).Length
$sha = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLowerInvariant()
@(
    'RESULT: PASS'
    'Format: UNSIGNED iOS IPA - external signing REQUIRED'
    ('Size bytes: ' + $size)
    ('SHA256: ' + $sha)
    ('Private output: ' + $output)
) | Out-File -FilePath $report -Encoding utf8
Write-Host ('PASS: Unsigned native IPA created: ' + $output)
Write-Host ('SHA256: ' + $sha)
Write-Host ('Result report: ' + $report)
Write-Host 'SIGNING REQUIRED: Use a compatible external signing/sideloading tool.'
exit 0
