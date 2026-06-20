# Performance regression check script for TurboScript JIT (PowerShell)

param(
    [string]$BuildDir = "..\..\build",
    [double]$MinSpeedup = 8.0,
    [double]$RegressionThreshold = 0.9
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildPath = Join-Path $ScriptDir $BuildDir | Resolve-Path
$BenchExec = Join-Path $BuildPath "bin\Release\bench_turbo_script_mir.exe"

# Colors
function Write-ColorOutput {
    param(
        [string]$Message,
        [string]$Color = "White"
    )
    Write-Host $Message -ForegroundColor $Color
}

Write-Host "========================================"
Write-Host "TurboScript JIT Performance Regression Check"
Write-Host "========================================"
Write-Host ""

# Check if benchmark executable exists
if (-not (Test-Path $BenchExec)) {
    Write-ColorOutput "Error: Benchmark executable not found at $BenchExec" "Red"
    Write-Host "Please build the project first:"
    Write-Host "  cd build"
    Write-Host "  cmake --build . --config Release --target bench_turbo_script_mir"
    exit 1
}

# Run benchmark
Write-Host "Running performance benchmarks..."
try {
    $BenchOutput = & $BenchExec 2>&1 | Out-String
} catch {
    Write-ColorOutput "Error: Benchmark execution failed" "Red"
    Write-Host $_.Exception.Message
    exit 1
}

Write-Host $BenchOutput
Write-Host ""

# Extract speedup values
$SpeedupMatches = [regex]::Matches($BenchOutput, "Speedup:\s+(\d+\.?\d*)x")

if ($SpeedupMatches.Count -eq 0) {
    Write-ColorOutput "Error: Could not extract speedup values from benchmark output" "Red"
    exit 1
}

$Speedups = @()
foreach ($match in $SpeedupMatches) {
    $Speedups += [double]$match.Groups[1].Value
}

# Calculate statistics
$Total = ($Speedups | Measure-Object -Sum).Sum
$Count = $Speedups.Count
$AvgSpeedup = [math]::Round($Total / $Count, 2)
$MinFound = ($Speedups | Measure-Object -Minimum).Minimum
$MaxFound = ($Speedups | Measure-Object -Maximum).Maximum

Write-Host "========================================"
Write-Host "Performance Statistics:"
Write-Host "========================================"
Write-Host "  Benchmarks run: $Count"
Write-Host "  Average speedup: ${AvgSpeedup}x"
Write-Host "  Min speedup: ${MinFound}x"
Write-Host "  Max speedup: ${MaxFound}x"
Write-Host ""

# Check for regression
$Regression = $false

if ($MinFound -lt $MinSpeedup) {
    Write-ColorOutput "❌ REGRESSION DETECTED!" "Red"
    Write-Host "   Minimum speedup ($MinFound) is below threshold ($MinSpeedup)"
    $Regression = $true
}

if ($AvgSpeedup -lt $MinSpeedup) {
    Write-ColorOutput "❌ REGRESSION DETECTED!" "Red"
    Write-Host "   Average speedup ($AvgSpeedup) is below threshold ($MinSpeedup)"
    $Regression = $true
}

# Compare with baseline if available
$BaselineFile = Join-Path $ScriptDir "performance_baseline.txt"

if (Test-Path $BaselineFile) {
    Write-Host "Comparing with baseline..."
    $BaselineAvg = [double](Get-Content $BaselineFile)
    
    $Ratio = [math]::Round($AvgSpeedup / $BaselineAvg, 2)
    
    if ($Ratio -lt $RegressionThreshold) {
        Write-ColorOutput "❌ REGRESSION DETECTED!" "Red"
        Write-Host "   Current average ($AvgSpeedup) is ${Ratio}x of baseline ($BaselineAvg)"
        Write-Host "   This is below the acceptable threshold (${RegressionThreshold}x)"
        $Regression = $true
    } else {
        Write-ColorOutput "✓ Performance is acceptable" "Green"
        Write-Host "   Current average ($AvgSpeedup) is ${Ratio}x of baseline ($BaselineAvg)"
    }
} else {
    Write-ColorOutput "Warning: No baseline file found at $BaselineFile" "Yellow"
    Write-Host "Creating baseline with current results..."
    $AvgSpeedup | Out-File -FilePath $BaselineFile -Encoding ASCII
    Write-Host "Baseline saved: ${AvgSpeedup}x"
}

Write-Host ""
Write-Host "========================================"

if (-not $Regression) {
    Write-ColorOutput "✓ All performance checks passed!" "Green"
    exit 0
} else {
    Write-ColorOutput "✗ Performance regression detected!" "Red"
    Write-Host ""
    Write-Host "Possible causes:"
    Write-Host "  1. Unsupported nodes or helper paths in hot code (check test_turbo_script_mir)"
    Write-Host "  2. Compilation overhead increased"
    Write-Host "  3. MIR backend regression"
    Write-Host "  4. System load (try running again)"
    Write-Host ""
    Write-Host "Debugging steps:"
    Write-Host "  1. Run: .\test_turbo_script_mir.exe"
    Write-Host "  2. Check: git diff HEAD~1 turbo_script\src\turbo_script_mir.c"
    Write-Host "  3. Profile: Use Visual Studio Profiler"
    exit 1
}
