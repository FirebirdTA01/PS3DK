param([string]$Source, [string]$Output)
$ErrorActionPreference = 'Stop'
Add-Type -Path $Source -OutputAssembly $Output -OutputType ConsoleApplication
