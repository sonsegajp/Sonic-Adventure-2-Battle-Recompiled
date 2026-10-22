# Installs what the HD texture builder (build_pack.py) needs - run by the launcher's HD TEXTURES
# page, once - in %LOCALAPPDATA%\SA2BRecompiled\upscaler (a short path: PyTorch's files nest so deep
# that under a game folder in Downloads they pass Windows' 260-character limit and pip fails):
#   python\   a private Python 3.11 (python.org's embeddable build) with PyTorch - the CUDA build when
#             there is an NVIDIA graphics card, else the CPU one - and the other packages
#   models\   HDcube 4 Compact and its alpha model, from their author's GitHub release (the model's
#             licence does not allow passing it on, so it is fetched from there, not shipped)
# Progress goes to setup_status.txt beside this script: "<running|finished|failed> <percent> <note>".
param([string]$Dir = $PSScriptRoot, [string]$Target = (Join-Path $env:LOCALAPPDATA 'SA2BRecompiled\upscaler'))
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'  # (Invoke-WebRequest's progress bar slows downloads a lot)
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$status = Join-Path $Dir 'setup_status.txt'
function Say([string]$state, [int]$pct, [string]$note) { Set-Content -Path $status -Value "$state $pct $note" -Encoding ascii }

try {
    $py = Join-Path $Target 'python'
    $exe = Join-Path $py 'python.exe'
    $tmp = Join-Path $Target 'download'
    New-Item -ItemType Directory -Force $tmp | Out-Null

    if (-not (Test-Path (Join-Path $py 'Scripts\pip.exe'))) {
        Say running 2 'Downloading Python...'
        $zip = Join-Path $tmp 'python.zip'
        Invoke-WebRequest 'https://www.python.org/ftp/python/3.11.9/python-3.11.9-embed-amd64.zip' -OutFile $zip
        Expand-Archive $zip -DestinationPath $py -Force
        # the embeddable build ignores site-packages until its path file allows it
        $pth = Join-Path $py 'python311._pth'
        (Get-Content $pth) -replace '^#\s*import site', 'import site' | Set-Content $pth -Encoding ascii
        Say running 6 'Setting up pip...'
        Invoke-WebRequest 'https://bootstrap.pypa.io/get-pip.py' -OutFile (Join-Path $tmp 'get-pip.py')
        & $exe (Join-Path $tmp 'get-pip.py') --no-warn-script-location
        if ($LASTEXITCODE) { throw 'pip did not install' }
    }

    $nvidia = @(Get-CimInstance Win32_VideoController | Where-Object { $_.Name -match 'NVIDIA' }).Count -gt 0
    # CUDA 12.6 builds still run on older cards (GTX 900 series and up)
    $index = if ($nvidia) { 'https://download.pytorch.org/whl/cu126' } else { 'https://download.pytorch.org/whl/cpu' }
    Say running 10 $(if ($nvidia) { 'Downloading PyTorch for your NVIDIA card (about 3 GB)...' } else { 'Downloading PyTorch (no NVIDIA card: the processor does the work)...' })
    & $exe -m pip install --no-warn-script-location torch torchvision --index-url $index
    if ($LASTEXITCODE) { throw 'PyTorch did not install' }
    Say running 80 'Installing the other packages...'
    & $exe -m pip install --no-warn-script-location numpy pillow xxhash safetensors einops typing_extensions
    if ($LASTEXITCODE) { throw 'the packages did not install' }
    & $exe -m pip install --no-warn-script-location --no-deps spandrel
    if ($LASTEXITCODE) { throw 'spandrel did not install' }

    Say running 90 'Downloading the HDcube model...'
    $mzip = Join-Path $tmp 'hdcube.zip'
    Invoke-WebRequest 'https://github.com/Venomalia/HDcube/releases/download/4.0/4xHDcube4Compact.zip' -OutFile $mzip
    $models = Join-Path $Target 'models'
    Expand-Archive $mzip -DestinationPath $models -Force
    if (-not (Test-Path (Join-Path $models 'Models\HDcube\4xHDCube4Compact.pth'))) { throw 'the model archive had no 4xHDCube4Compact.pth' }

    Remove-Item -Recurse -Force $tmp
    Say finished 100 'The upscaler is installed.'
} catch {
    Say failed 0 "$($_.Exception.Message)"
    exit 1
}
