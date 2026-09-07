# Windows counterpart to mac/start.command. All arguments go to backend.launch.
# From the repo root: .\window\start.cmd --demo
$ErrorActionPreference = 'Stop'
$launchArguments = @($args)
$env:PYTHONUTF8 = '1'
$env:PYTHONUNBUFFERED = '1'
$repoRoot = Split-Path -Parent $PSScriptRoot
$backendPython = Join-Path $repoRoot 'backend\.venv\Scripts\python.exe'
$launchExit = 0

Push-Location -LiteralPath $repoRoot
try {
    if ($launchArguments -contains '--dev') {
        throw 'Windows launcher serves the built dashboard. For hot reload, run npm run dev from dashboard in a second terminal.'
    }

    if (-not (Test-Path -LiteralPath $backendPython)) {
        Write-Host 'Creating backend Python environment...'
        if (Get-Command py.exe -ErrorAction SilentlyContinue) {
            & py.exe -3 -m venv backend/.venv
        } elseif (Get-Command python.exe -ErrorAction SilentlyContinue) {
            & python.exe -m venv backend/.venv
        } else {
            throw 'Python is missing. Install Python 3.12 or newer and reopen this launcher.'
        }
        if ($LASTEXITCODE -ne 0) { throw 'Could not create the Python environment.' }
    }

    & $backendPython -c 'import sys; sys.exit(0 if sys.version_info >= (3, 12) else 1)'
    if ($LASTEXITCODE -ne 0) { throw 'The backend requires Python 3.12 or newer.' }
    $dependencyCheck = @'
import sys
try:
    import fastapi, uvicorn, serial, websockets
except ImportError:
    sys.exit(1)
'@
    & $backendPython -c $dependencyCheck
    if ($LASTEXITCODE -ne 0) {
        Write-Host 'Installing backend dependencies...'
        & $backendPython -m pip install --disable-pip-version-check fastapi uvicorn pyserial websockets
        if ($LASTEXITCODE -ne 0) { throw 'Backend dependency installation failed.' }
    }

    if (-not (Get-Command npm.cmd -ErrorAction SilentlyContinue)) {
        throw 'Node.js/npm is missing. Install Node.js 22.12 or newer and reopen this launcher.'
    }
    Push-Location -LiteralPath (Join-Path $repoRoot 'dashboard')
    try {
        $dependencyStamp = 'node_modules/.ground-station-lock.sha256'
        $hasher = [System.Security.Cryptography.SHA256]::Create()
        try {
            $lockBytes = [System.IO.File]::ReadAllBytes((Join-Path $repoRoot 'dashboard/package-lock.json'))
            $lockHash = [BitConverter]::ToString($hasher.ComputeHash($lockBytes)).Replace('-', '')
        } finally {
            $hasher.Dispose()
        }
        $installedHash = if (Test-Path -LiteralPath $dependencyStamp) {
            (Get-Content -LiteralPath $dependencyStamp -Raw).Trim()
        } else { '' }
        if ($installedHash -ne $lockHash) {
            Write-Host 'Installing dashboard dependencies from package-lock.json...'
            & npm.cmd ci
            if ($LASTEXITCODE -ne 0) { throw 'Dashboard dependency installation failed.' }
            Set-Content -LiteralPath $dependencyStamp -Value $lockHash
        }
        Write-Host 'Building dashboard...'
        & npm.cmd run build
        if ($LASTEXITCODE -ne 0) { throw 'Dashboard build failed.' }
    } finally {
        Pop-Location
    }

    Write-Host 'Starting backend and dashboard. Press Ctrl+C to stop.'
    if ($launchArguments -contains '--demo') {
        Write-Host 'DEMO MODE: telemetry is simulated, not from a flight computer.' -ForegroundColor Yellow
    }
    & $backendPython -m backend.launch @launchArguments
    $launchExit = $LASTEXITCODE
} catch {
    Write-Host "Startup failed: $_" -ForegroundColor Red
    $launchExit = 1
} finally {
    Pop-Location
}
exit $launchExit
