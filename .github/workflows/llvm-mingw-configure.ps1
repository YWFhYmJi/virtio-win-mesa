# LLVM-MinGW automatic download, extraction and PATH configuration script
Write-Host "Fetching latest LLVM-MinGW download link..." -ForegroundColor Cyan

# 1. Get download link with GitHub Actions compatibility
try {
    # Ensure TLS 1.2 for GitHub API (required in CI environments)
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    
    # Use -UseBasicParsing to avoid IE engine dependency in headless environments
    $response = Invoke-WebRequest -Uri "https://api.github.com/repos/mstorsjo/llvm-mingw/releases/latest" `
        -UseBasicParsing `
        -Headers @{
            "Accept" = "application/vnd.github.v3+json"
            "User-Agent" = "PowerShell-GitHubActions"
        }
    
    $downloadUrl = $response.Content | 
        jq -r '.assets[] | select(.name | test(""msvcrt-x86_64"")) | .browser_download_url' | 
        Select-Object -First 1
    
    if (-not $downloadUrl) {
        Write-Error "No asset file matching 'msvcrt-x86_64' found"
        exit 1
    }
    
    Write-Host "Download link found: $downloadUrl" -ForegroundColor Green
}
catch {
    Write-Error "Failed to fetch download link: $_"
    Write-Host "Error details:" -ForegroundColor Yellow
    Write-Host "Exception Type: $($_.Exception.GetType().FullName)" -ForegroundColor Yellow
    Write-Host "Exception Message: $($_.Exception.Message)" -ForegroundColor Yellow
    
    # Alternative method using Invoke-RestMethod
    Write-Host "Trying alternative method with Invoke-RestMethod..." -ForegroundColor Cyan
    try {
        $releaseInfo = Invoke-RestMethod -Uri "https://api.github.com/repos/mstorsjo/llvm-mingw/releases/latest" `
            -Headers @{ "Accept" = "application/vnd.github.v3+json" }
        
        $downloadUrl = $releaseInfo.assets | 
            Where-Object { $_.name -match "msvcrt-x86_64" } | 
            Select-Object -First 1 -ExpandProperty browser_download_url
        
        if ($downloadUrl) {
            Write-Host "Download link found (alternative method): $downloadUrl" -ForegroundColor Green
        } else {
            Write-Error "Alternative method also failed to find matching asset"
            exit 1
        }
    }
    catch {
        Write-Error "All methods failed to fetch download link. Last error: $_"
        exit 1
    }
}

# Check if already processed
$zipFile = Split-Path $downloadUrl -Leaf
$destination = Join-Path (Get-Location) ($zipFile -replace '\.zip$', '')

# Check if destination folder already exists
if (Test-Path $destination -PathType Container) {
    Write-Host "Destination folder already exists: $destination" -ForegroundColor Yellow
    Write-Host "Skipping download and extraction, proceeding to PATH configuration..." -ForegroundColor Yellow
    
    # Skip to PATH configuration section
    # The following flags will help control the flow
    $skipDownload = $true
    $skipToPathConfig = $true
} 
# Check if zip file exists (but folder doesn't)
elseif (Test-Path $zipFile -PathType Leaf) {
    Write-Host "Zip file already exists: $zipFile" -ForegroundColor Yellow
    Write-Host "Skipping download, proceeding with extraction only..." -ForegroundColor Yellow
    
    # Only need extraction, not download
    $skipDownload = $true
    $skipToPathConfig = $false
}
else {
    Write-Host "No existing files found, proceeding with full download and extraction..." -ForegroundColor Cyan
    $skipDownload = $false
    $skipToPathConfig = $false
}

# 2. Download file (conditional)
if (-not $skipDownload) {
    Write-Host "Downloading file: $zipFile" -ForegroundColor Cyan
    
    try {
        Invoke-WebRequest -Uri $downloadUrl -OutFile $zipFile
        Write-Host "Download completed" -ForegroundColor Green
    } catch {
        Write-Error "Download failed: $_"
        exit 1
    }
} else {
    Write-Host "Download skipped (file already exists)" -ForegroundColor Green
}

# 3. Extract file (only if not skipping to PATH config)
if (-not $skipToPathConfig) {
    Write-Host "Extracting to: $destination" -ForegroundColor Cyan
    
    try {
        # Delete destination directory if it already exists (shouldn't happen with our checks)
        if (Test-Path $destination) {
            Remove-Item $destination -Recurse -Force
        }
        
        Expand-Archive -Path $zipFile -DestinationPath $destination -Force
        Write-Host "Extraction completed" -ForegroundColor Green
    } catch {
        Write-Error "Extraction failed: $_"
        exit 1
    }
} else {
    Write-Host "Extraction skipped (folder already exists)" -ForegroundColor Green
}

# 4. Find bin directory
Write-Host "Looking for bin directory..." -ForegroundColor Cyan

# Get contents of extracted directory
$items = Get-ChildItem -Path $destination

# Find first folder containing a bin subdirectory
$binPath = $null
if ($items.count -eq 1) {
    $item = $items[0]
    if ($item.PSIsContainer) {
        $potentialBinPath = Join-Path $item.FullName "bin"
        if (Test-Path $potentialBinPath -PathType Container) {
            $binPath = $potentialBinPath
        }
    }
}

# If not found, check for bin in root directory
if (-not $binPath) {
    $potentialBinPath = Join-Path $destination "bin"
    if (Test-Path $potentialBinPath -PathType Container) {
        $binPath = $potentialBinPath
    }
}

if (-not $binPath) {
    Write-Error "Bin directory not found, please manually check the extracted folder structure"
    Write-Host "Extracted directory contents:" -ForegroundColor Yellow
    Get-ChildItem -Path $destination -Recurse | Select-Object FullName | Format-Table -AutoSize
    exit 1
}

Write-Host "Bin directory found: $binPath" -ForegroundColor Green

# 5. Add to PATH environment variable
Write-Host "Configuring PATH environment variable..." -ForegroundColor Cyan

# Get current user PATH
$userPath = [Environment]::GetEnvironmentVariable("PATH", "User")
$pathSeparator = ';'

# Check if already exists
$pathEntries = $userPath -split $pathSeparator
if ($pathEntries -contains $binPath) {
    Write-Host "Bin directory already in PATH, no need to add again" -ForegroundColor Yellow
} else {
    # Add to user PATH
    $newUserPath = if ($userPath) { "$userPath$pathSeparator$binPath" } else { $binPath }
    
    try {
        [Environment]::SetEnvironmentVariable("PATH", $newUserPath, "User")
        Write-Host "Permanently added to user PATH environment variable" -ForegroundColor Green
    } catch {
        Write-Warning "Failed to permanently modify PATH: $_"
        Write-Host "Will only add to current session PATH" -ForegroundColor Yellow
    }
}

# Add to current process PATH (immediately effective)
$env:Path += "$pathSeparator$binPath"
Write-Host "Added to current session PATH" -ForegroundColor Green

# 6. Cleanup and verification
Write-Host "`nVerifying configuration..." -ForegroundColor Cyan

# Check if accessible from PATH
$binDir = Get-Item $binPath
$exeFiles = Get-ChildItem -Path $binPath -Filter "*.exe"

Write-Host "`nConfiguration completed!" -ForegroundColor Green
Write-Host "=========================================" -ForegroundColor Cyan
Write-Host "Downloaded file: $zipFile" -ForegroundColor White
Write-Host "Extraction directory: $destination" -ForegroundColor White
Write-Host "Bin directory: $binPath" -ForegroundColor White
Write-Host "Avaliable tools: $($exeFiles.Name -join ', ')" -ForegroundColor White
Write-Host "=========================================" -ForegroundColor Cyan
