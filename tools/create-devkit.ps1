#
# Assembles a dev kit for both AF_XDP client and XDP driver development.
# Code must be built before running this script.
#
#  TODO:
# -xdp.cat (originally generated during build)
# -sha2-testroot.cer (originally copied from BASEDIR\tools)
# -pktcmd.exe / pktcmd.pdb (needs vcxproj)
# -xskbench.exe / xskbench.pdb (needs vcxproj)
# -convert from CIL to native? These commands don't work:
#   link.exe /cvtcil /out:$dstPath\lib\msxdp.lib  ".\artifacts\bin\$($Platform)_$($Flavor)\msxdp.lib"
#   link.exe /cvtcil /out:$dstPath\lib\xdpnmr.lib ".\build\$($Platform)_$($Flavor)\bin\xdpnmr.lib"
#

param (
    [ValidateSet("x64")]
    [Parameter(Mandatory=$false)]
    [string]$Platform = "x64",

    [ValidateSet("Debug", "Release")]
    [Parameter(Mandatory=$false)]
    [string]$Flavor = "Release"
)

$dstPath = "$(git rev-parse --short HEAD)-$Platform"

New-Item -Path $dstPath -ItemType Directory > $null

git rev-parse HEAD >> $dstPath\info.txt
git branch --show-current >> $dstPath\info.txt
$Platform >> $dstPath\info.txt

copy readme.md $dstPath

New-Item -Path $dstPath\bin -ItemType Directory > $null
copy "artifacts\bin\$($Platform)_$($Flavor)\xdp.inf"   $dstPath\bin
copy "artifacts\bin\$($Platform)_$($Flavor)\xdp.sys"   $dstPath\bin
copy "artifacts\bin\$($Platform)_$($Flavor)\msxdp.dll" $dstPath\bin

New-Item -Path $dstPath\symbols -ItemType Directory > $null
copy "artifacts\bin\$($Platform)_$($Flavor)\xdp.pdb"   $dstPath\symbols
copy "artifacts\bin\$($Platform)_$($Flavor)\msxdp.pdb" $dstPath\symbols

New-Item -Path $dstPath\include -ItemType Directory > $null
copy -Recurse published\external\* $dstPath\include

New-Item -Path $dstPath\lib -ItemType Directory > $null
copy "artifacts\bin\$($Platform)_$($Flavor)\msxdp.lib" $dstPath\lib
copy "build\$($Platform)_$($Flavor)\bin\xdpnmr.lib"  $dstPath\lib

New-Item -Path $dstPath\samples -ItemType Directory > $null
New-Item -Path $dstPath\samples\xdpmp -ItemType Directory > $null
copy test\xdpmp\*.c $dstPath\samples\xdpmp
copy test\xdpmp\*.h $dstPath\samples\xdpmp
copy test\xdpmp\inf\xdpmp.inx $dstPath\samples\xdpmp
