# Runs one test target of the Makefile (test-unit or test-console) and turns what failed into
# annotations of the workflow run.
#
# The log of a run needs a login to read; an annotation is on the run page and in the public
# API. So each FAIL (with the expected/got lines under it and the test program it came from)
# becomes one, a notice lists the suites that finished and describes the console they ran in,
# and the end of the log comes last, because a test that hangs or crashes leaves no FAIL line -
# only a log that stops.
#
# Usage, from the root of the repository:   .github/run-tests.ps1 test-unit

param(
  [Parameter(Mandatory = $true)]
  [string]$Target
)

$ErrorActionPreference = 'Continue'
Write-Output '::add-matcher::.github/clang.json'

$log = "$Target.log"
mingw32-make -k $Target WERROR=1 2>&1 | Tee-Object -FilePath $log
$rc = $LASTEXITCODE

# the workflow commands put a newline, a carriage return and a percent sign in %-escapes
function Escape($text) {
  $text -replace '%', '%25' -replace "`r", '%0D' -replace "`n", '%0A'
}

$lines  = @(Get-Content $log)
$suite  = ''
$fails  = 0
$passed = @()
for ($i = 0; $i -lt $lines.Count; $i++) {
  $line = $lines[$i]
  if ($line -match '^(tests\\\S+\.exe)') { $suite = $Matches[1] }
  if ($line -match 'passed|^SKIP|^console:') { $passed += $line.Trim() }
  if ($line -match '^FAIL') {
    $fails++
    if ($fails -le 8) {
      $last = [Math]::Min($i + 6, $lines.Count - 1)
      $text = ($lines[$i..$last] -join "`n")
      Write-Output ("::error title=FAIL in {0}::{1}" -f $suite, (Escape $text))
    }
  }
}

if ($passed.Count) {
  Write-Output ("::notice title={0}: suites that finished::{1}" -f $Target, (Escape ($passed -join "`n")))
}
if ($rc -ne 0) {
  $tail = ($lines | Select-Object -Last 25) -join "`n"
  Write-Output ("::error title={0}: exit code {1}, {2} FAIL line(s); the end of the log::{3}" -f $Target, $rc, $fails, (Escape $tail))
}
exit $rc
