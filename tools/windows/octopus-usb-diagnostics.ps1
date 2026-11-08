<#
.SYNOPSIS
    Read-only report of how Windows sees a connected iPhone over USB, for Octopus development.

.DESCRIPTION
    Lists the Apple USB devices (vendor ID 05AC) and Windows Portable Devices (WPD) entries Windows
    has, with their instance IDs, hardware and compatible IDs (which give each USB interface's
    class), drivers and problem codes; the WPD and Apple services; the storages File Explorer shows
    for each portable device (names and counts only); recent Plug and Play events; and whether the
    optional capture tools USBPcap and Wireshark are installed.

    It changes nothing: no driver, service, device or setting is installed, enabled, disabled or
    restarted, and nothing is read from the iPhone's files. No administrator rights are needed.
    Serial numbers in instance IDs are shortened unless you pass -ShowSerials. The report is printed
    and saved as a text file in the current folder, or to -OutFile.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\tools\windows\octopus-usb-diagnostics.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\octopus-usb-diagnostics.ps1 -OutFile "$env:USERPROFILE\Desktop\usb.txt"
#>
[CmdletBinding()]
param(
    [string]$OutFile = (Join-Path -Path (Get-Location) -ChildPath ("octopus-usb-diagnostics-{0}.txt" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))),
    [switch]$ShowSerials
)

Set-StrictMode -Version 2
$ErrorActionPreference = 'Continue'
$script:Report = New-Object System.Collections.Generic.List[string]

function Add-Line {
    param([string]$Text = '')
    $script:Report.Add($Text)
    Write-Host $Text
}

function Add-Section {
    param([string]$Title)
    Add-Line ''
    Add-Line ('== {0} {1}' -f $Title, ('=' * [Math]::Max(3, 70 - $Title.Length)))
}

# USB\VID_05AC&PID_12A8\00008110000649EA2E85401E -> USB\VID_05AC&PID_12A8\0000...401E
function Format-Id {
    param([string]$Id)
    if ($ShowSerials -or [string]::IsNullOrEmpty($Id)) { return $Id }
    return ($Id -replace '(?<=\\)([0-9A-Za-z]{4})[0-9A-Za-z]{8,}([0-9A-Za-z]{4})$', '$1...$2')
}

# What a USB compatible ID says about the interface.
function Describe-Interface {
    param([string[]]$CompatibleIds)
    foreach ($id in $CompatibleIds) {
        if ($id -match 'Class_06&SubClass_01&Prot_01') { return 'Still Image (PTP): the interface Windows Portable Devices uses, and the one Octopus targets' }
        if ($id -match 'Class_06') { return 'Still Image class' }
        if ($id -match 'Class_FF') { return 'Vendor-specific (Apple): usbmux, used by Apple Devices / iTunes' }
        if ($id -match 'Class_02|Class_0A') { return 'Communications (network sharing)' }
        if ($id -match 'Class_01') { return 'Audio' }
    }
    return $null
}

Add-Line ('Octopus USB diagnostics, {0}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz'))
Add-Line ('Windows {0}; PowerShell {1}' -f [Environment]::OSVersion.Version, $PSVersionTable.PSVersion)
Add-Line 'Read-only: nothing on this PC or the iPhone is changed.'

# ---- Devices
Add-Section 'Apple and portable devices'
$devices = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object {
        $_.InstanceId -match 'VID_05AC' -or $_.Class -eq 'WPD' -or $_.FriendlyName -match 'Apple|iPhone|MTP|PTP|Portable'
    } | Sort-Object InstanceId)
if ($devices.Count -eq 0) {
    Add-Line 'No Apple or portable devices are present. Plug the iPhone in, unlock it, and tap Trust if asked.'
}
$keys = @('DEVPKEY_Device_HardwareIds', 'DEVPKEY_Device_CompatibleIds', 'DEVPKEY_Device_Service',
    'DEVPKEY_Device_DriverProvider', 'DEVPKEY_Device_DriverVersion', 'DEVPKEY_Device_DriverDate',
    'DEVPKEY_Device_DriverInfPath', 'DEVPKEY_Device_Parent', 'DEVPKEY_Device_BusReportedDeviceDesc',
    'DEVPKEY_Device_ProblemCode', 'DEVPKEY_Device_LocationInfo')
foreach ($device in $devices) {
    Add-Line ''
    Add-Line ('{0}  [{1}]  {2}' -f $device.FriendlyName, $device.Class, $device.Status)
    Add-Line ('  InstanceId: {0}' -f (Format-Id $device.InstanceId))
    $compatible = @()
    $properties = @(Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName $keys -ErrorAction SilentlyContinue)
    foreach ($property in $properties) {
        $value = $property.Data
        if ($null -eq $value) { continue }
        if ($property.KeyName -eq 'DEVPKEY_Device_CompatibleIds') { $compatible = @($value) }
        if ($value -is [array]) { $value = ($value | ForEach-Object { Format-Id ([string]$_) }) -join '; ' }
        elseif ($property.KeyName -eq 'DEVPKEY_Device_Parent') { $value = Format-Id ([string]$value) }
        if ([string]$value -eq '') { continue }
        Add-Line ('  {0}: {1}' -f ($property.KeyName -replace '^DEVPKEY_Device_', ''), $value)
    }
    $meaning = Describe-Interface $compatible
    if ($meaning) { Add-Line ('  Interface: {0}' -f $meaning) }
}

# ---- Drivers
Add-Section 'Signed drivers for those devices'
$ids = @($devices | ForEach-Object { $_.InstanceId })
$drivers = @(Get-CimInstance -ClassName Win32_PnPSignedDriver -ErrorAction SilentlyContinue | Where-Object { $ids -contains $_.DeviceID })
if ($drivers.Count -eq 0) { Add-Line 'None found.' }
foreach ($driver in $drivers) {
    Add-Line ('{0}' -f (Format-Id $driver.DeviceID))
    Add-Line ('  {0} {1}, {2}, {3}, signed by {4}' -f $driver.DriverProviderName, $driver.DriverVersion, $driver.InfName,
        $driver.DriverDate, $driver.Signer)
}

# ---- Services
Add-Section 'Services'
foreach ($name in @('WpdBusEnum', 'WUDFSvc', 'Apple Mobile Device Service', 'AppleMobileDeviceService', 'USBPcap')) {
    $service = Get-Service -Name $name -ErrorAction SilentlyContinue
    if ($service) { Add-Line ('{0} ({1}): {2}, start {3}' -f $service.DisplayName, $service.Name, $service.Status, $service.StartType) }
}

# ---- What File Explorer shows
Add-Section 'Portable devices in File Explorer (This PC)'
try {
    $shell = New-Object -ComObject Shell.Application
    $thisPC = $shell.NameSpace(17)
    $shown = 0
    foreach ($item in @($thisPC.Items())) {
        if ($item.Path -notmatch 'usb#vid_') { continue }
        $shown++
        Add-Line ('{0}  ({1})' -f $item.Name, $item.Type)
        try {
            foreach ($storage in @($item.GetFolder.Items())) {
                $count = @($storage.GetFolder.Items()).Count
                Add-Line ('  Storage: {0}, {1} items at its top level' -f $storage.Name, $count)
            }
        } catch {
            Add-Line ('  Storages could not be listed: {0}' -f $_.Exception.Message)
        }
    }
    if ($shown -eq 0) { Add-Line 'No portable device is listed under This PC. Unlock the iPhone and check Trust.' }
} catch {
    Add-Line ('File Explorer could not be queried: {0}' -f $_.Exception.Message)
}

# ---- Recent Plug and Play events for Apple devices
Add-Section 'Recent Plug and Play events (Apple devices)'
try {
    $events = @(Get-WinEvent -LogName 'Microsoft-Windows-Kernel-PnP/Configuration' -MaxEvents 300 -ErrorAction Stop |
            Where-Object { $_.Message -match 'VID_05AC' } | Select-Object -First 15)
    if ($events.Count -eq 0) { Add-Line 'None in the last 300 events.' }
    foreach ($entry in $events) {
        $first = ($entry.Message -split "`r?`n")[0]
        Add-Line ('{0}  {1}  {2}' -f $entry.TimeCreated.ToString('yyyy-MM-dd HH:mm:ss'), $entry.Id, (Format-Id $first))
    }
} catch {
    Add-Line ('The event log could not be read: {0}' -f $_.Exception.Message)
}

# ---- Optional tools
Add-Section 'Optional capture tools (not needed to use Octopus)'
$tools = @(
    @{ Name = 'USBPcap'; Paths = @("$env:ProgramFiles\USBPcap\USBPcapCMD.exe") },
    @{ Name = 'Wireshark'; Paths = @("$env:ProgramFiles\Wireshark\Wireshark.exe", "${env:ProgramFiles(x86)}\Wireshark\Wireshark.exe") }
)
foreach ($tool in $tools) {
    $found = @($tool.Paths | Where-Object { $_ -and (Test-Path -LiteralPath $_) })
    Add-Line ('{0}: {1}' -f $tool.Name, $(if ($found.Count) { 'installed at ' + $found[0] } else { 'not installed' }))
}

try {
    $script:Report | Set-Content -LiteralPath $OutFile -Encoding UTF8
    Write-Host ''
    Write-Host ('Saved to {0}' -f $OutFile)
} catch {
    Write-Host ('The report could not be saved: {0}' -f $_.Exception.Message)
}
