# Run as Administrator
if (-not ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole] "Administrator")) {
  Write-Error "Please run this script in an elevated (Administrator) PowerShell."
  exit 1
}

Write-Output "Enabling WSL and Virtual Machine Platform features..."
# Enable required Windows features
dism.exe /online /enable-feature /featurename:Microsoft-Windows-Subsystem-Linux /all /norestart
dism.exe /online /enable-feature /featurename:VirtualMachinePlatform /all /norestart

Write-Output "Setting WSL default version to 2 (may require restart)..."
wsl --set-default-version 2

Write-Output "Installing Ubuntu (may open Microsoft Store or use web download)..."
wsl --install -d Ubuntu

Write-Output "If the previous command fails, please install Ubuntu from the Microsoft Store manually.
After Ubuntu is installed, open the Ubuntu terminal and run: /mnt/d/自制操作系统/scripts/wsl-setup.sh"
