$ErrorActionPreference = "Stop"

function Test-PathPairCorpusShaderName([string]$name) {
    return ($name -like "*.fcg") -or ($name -like "*_f.cg")
}

function Get-PathPairCorpusFiles([string]$Root, [string]$Manifest = "") {
    if (-not (Test-Path -LiteralPath $Root -PathType Container)) {
        throw "path-pair corpus root not a directory: $Root"
    }
    $rootPath = [System.IO.Path]::GetFullPath((Get-Item -LiteralPath $Root).FullName).TrimEnd('\')

    if ($Manifest) {
        if (-not (Test-Path -LiteralPath $Manifest -PathType Leaf)) {
            throw "path-pair corpus manifest not found: $Manifest"
        }
        $rows = @()
        $seen = @{}
        foreach ($line in (Get-Content -LiteralPath $Manifest)) {
            $rel = $line.Trim()
            if (-not $rel -or $rel.StartsWith("#")) { continue }
            $rel = $rel.Replace('\', '/')
            $parts = @($rel -split '/')
            if ([System.IO.Path]::IsPathRooted($rel) -or ($parts | Where-Object { $_ -eq ".." }).Count -ne 0) {
                throw "path-pair corpus manifest path must be root-relative: $rel"
            }
            if ($seen.ContainsKey($rel)) {
                throw "path-pair corpus manifest lists duplicate shader: $rel"
            }
            $seen[$rel] = 1
            if (-not (Test-PathPairCorpusShaderName ([System.IO.Path]::GetFileName($rel)))) {
                throw "path-pair corpus manifest lists non-fragment shader: $rel"
            }
            $full = [System.IO.Path]::GetFullPath((Join-Path $rootPath ($rel -replace '/', '\')))
            $rootPrefix = $rootPath.TrimEnd('\') + '\'
            if (-not $full.StartsWith($rootPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
                throw "path-pair corpus manifest path escapes root: $rel"
            }
            if (-not (Test-Path -LiteralPath $full -PathType Leaf)) {
                throw "path-pair corpus manifest lists missing shader: $rel"
            }
            $rows += [pscustomobject]@{
                File = Get-Item -LiteralPath $full
                RelativePath = $rel
            }
        }
        if ($rows.Count -eq 0) {
            throw "path-pair corpus manifest is empty: $Manifest"
        }
        return $rows
    }

    return @(Get-ChildItem -LiteralPath $rootPath -Recurse -File |
        Where-Object { Test-PathPairCorpusShaderName $_.Name } |
        Where-Object {
            $r = $_.FullName.Substring($rootPath.Length + 1).Replace('\', '/')
            -not ($r.StartsWith('build/') -or $r.Contains('/_work/') -or $r.StartsWith('_work/'))
        } |
        Sort-Object FullName |
        ForEach-Object {
            [pscustomobject]@{
                File = $_
                RelativePath = $_.FullName.Substring($rootPath.Length + 1).Replace('\', '/')
            }
        })
}
