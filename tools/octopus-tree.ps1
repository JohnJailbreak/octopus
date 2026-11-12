<#
.SYNOPSIS
  Write the Octopus volume's tree to a text file, as "size<TAB>/path" lines, the same format as a
  Caz tree, so the two can be compared directly.

.DESCRIPTION
  An MTP volume is not a drive letter, so dir /s and tree cannot read it; Windows' shell can. This
  walks it through Shell.Application.

  The first version recursed only when an item reported IsFolder, and on MTP volumes that can be
  false for real folders, so it wrote the top level and stopped. This descends into anything the
  shell can open as a folder, whatever IsFolder says, and reports per-folder counts on the console
  so an empty result is explained rather than silent.

  Keep the iPhone unlocked with Octopus on screen while this runs: authorization renews only while
  the app is in the foreground or within two minutes of leaving it, and a lapsed one makes every
  listing come back empty.

.PARAMETER Start
  A path inside the volume to begin from, such as "var/mobile/Media". Empty starts at the root.

.PARAMETER MaxDepth
  How many levels to descend below Start. MTP enumeration is slow; a whole root can take hours.

.PARAMETER Out
  The file to write. Defaults to octopus-tree.txt on the Desktop.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File octopus-tree.ps1 -Start "var/mobile" -MaxDepth 3
#>
param(
    [string]$Start = "",
    [int]$MaxDepth = 4,
    [string]$Out = (Join-Path ([Environment]::GetFolderPath('Desktop')) 'octopus-tree.txt')
)

$ErrorActionPreference = 'Continue'
$shell = New-Object -ComObject Shell.Application

# This PC is shell namespace 17.
$phone = $shell.Namespace(17).Items() | Where-Object { $_.Name -eq 'Apple iPhone' } | Select-Object -First 1
if (-not $phone) { Write-Error "No 'Apple iPhone' under This PC. Connect and unlock the phone."; exit 1 }
$volume = $phone.GetFolder.Items() | Where-Object { $_.Name -like 'Octopus*' } | Select-Object -First 1
if (-not $volume) { Write-Error "No Octopus volume. Enable the exposure toggle, then reconnect USB."; exit 1 }

# Descend to the starting folder, one component at a time.
$folder = $volume.GetFolder
$prefix = ''
foreach ($part in ($Start -split '/' | Where-Object { $_ })) {
    $next = $folder.Items() | Where-Object { $_.Name -eq $part } | Select-Object -First 1
    if (-not $next) { Write-Error "No '$part' under '/$($prefix.TrimStart('/'))'."; exit 1 }
    $folder = $next.GetFolder
    if (-not $folder) { Write-Error "'$part' cannot be opened as a folder."; exit 1 }
    $prefix = "$prefix/$part"
}

# Whether the shell can open an item as a folder. IsFolder alone is not trusted on MTP.
function Open-AsFolder($item) {
    try {
        $sub = $item.GetFolder
        if ($sub) { return $sub }
    } catch { }
    return $null
}

if (Test-Path $Out) { Remove-Item $Out }
$writer = [System.IO.StreamWriter]::new($Out, $false, [System.Text.UTF8Encoding]::new($false))
$written = 0
$empty = 0
try {
    # Iterative, so a deep tree cannot exhaust PowerShell's call depth.
    $stack = [System.Collections.Generic.Stack[object]]::new()
    $stack.Push(@($folder, $prefix, 0))
    while ($stack.Count -gt 0) {
        $frame = $stack.Pop()
        $current, $path, $depth = $frame[0], $frame[1], $frame[2]
        $items = @()
        try { $items = @($current.Items()) } catch { Write-Warning "Cannot list '$path': $_" }
        if ($items.Count -eq 0) { $empty++ }
        Write-Host ("{0,6} entries  {1}" -f $items.Count, ($(if ($path) { $path } else { '/' })))
        foreach ($item in $items) {
            $childPath = "$path/$($item.Name)"
            $size = 0
            try { $size = [int64]$item.Size } catch { }
            $writer.WriteLine("$size`t$childPath")
            $written++
            # Descend when the item says it is a folder, or when it reports no size and the shell can
            # open it as one: MTP folders report zero, and some report IsFolder false. A file with
            # content is never descended into, even if the shell would present it as browsable.
            if ($depth -lt $MaxDepth -and ($item.IsFolder -or $size -eq 0)) {
                $sub = Open-AsFolder $item
                if ($sub) { $stack.Push(@($sub, $childPath, $depth + 1)) }
            }
        }
    }
} finally {
    $writer.Dispose()
}
Write-Host ""
Write-Host "Wrote $written entries to $Out"
if ($empty -gt 0) {
    Write-Host "$empty folders listed nothing. If that includes folders you expect to hold files,"
    Write-Host "check the phone is unlocked with Octopus open: a lapsed authorization empties every listing."
}
