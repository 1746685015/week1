param(
	[Parameter(Mandatory = $true)]
	[string]$Executable,

	[Parameter(Mandatory = $true)]
	[string]$CsvPath,

	[Parameter(Mandatory = $true)]
	[string]$ReportPath,

	[string]$ErrorLogPath = '',
	[int]$RepeatCount = 1,
	[int]$PollMilliseconds = 50
)

$ErrorActionPreference = 'Stop'
$resolvedExecutable = (Resolve-Path -LiteralPath $Executable).Path
$resolvedCsvPath = (Resolve-Path -LiteralPath $CsvPath).Path
$resolvedReportPath = [System.IO.Path]::GetFullPath($ReportPath)
$processArguments = @($resolvedCsvPath, $RepeatCount.ToString(), $resolvedReportPath)
$resolvedErrorLogPath = $null
if ($ErrorLogPath -ne '')
{
	$resolvedErrorLogPath = [System.IO.Path]::GetFullPath($ErrorLogPath)
	$processArguments += $resolvedErrorLogPath
}

$startedAt = [System.Diagnostics.Stopwatch]::StartNew()
$process = Start-Process -FilePath $resolvedExecutable `
	-ArgumentList $processArguments `
	-PassThru `
	-WindowStyle Hidden

$peakWorkingSetBytes = 0L
$peakPrivateBytes = 0L
$lastWorkingSetBytes = 0L
$lastPrivateBytes = 0L
while (-not $process.WaitForExit($PollMilliseconds))
{
	$process.Refresh()
	$lastWorkingSetBytes = [int64]$process.WorkingSet64
	$lastPrivateBytes = [int64]$process.PrivateMemorySize64
	$peakWorkingSetBytes = [Math]::Max($peakWorkingSetBytes, [int64]$process.PeakWorkingSet64)
	$peakPrivateBytes = [Math]::Max($peakPrivateBytes, $lastPrivateBytes)
}
$startedAt.Stop()
$exitCode = $process.ExitCode

[pscustomobject]@{
	executable = $resolvedExecutable
	csv_path = $resolvedCsvPath
	report_path = $resolvedReportPath
	error_log_path = $resolvedErrorLogPath
	repeat_count = $RepeatCount
	poll_milliseconds = $PollMilliseconds
	exit_code = $exitCode
	wall_clock_milliseconds = [Math]::Round($startedAt.Elapsed.TotalMilliseconds, 3)
	peak_working_set_bytes = $peakWorkingSetBytes
	peak_working_set_mb = [Math]::Round($peakWorkingSetBytes / 1000000.0, 3)
	peak_working_set_mib = [Math]::Round($peakWorkingSetBytes / 1MB, 3)
	peak_private_bytes = $peakPrivateBytes
	last_observed_working_set_bytes = $lastWorkingSetBytes
	last_observed_private_bytes = $lastPrivateBytes
} | ConvertTo-Json

exit $exitCode
