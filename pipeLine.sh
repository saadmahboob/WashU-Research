#Usage: ./pipeLine.sh <location_of_data_to_use>/
#CSE Scale = 98.0
#DUC Scale = 73.5

#!/bin/bash


cd ~/Projects/3DscanData/DUC/Floor1/
find -type d -links 2 -exec mkdir -p "$1{}" \;
cd ~/Projects/c++/scanDensity
make
./csvToBinary -inFolder=$1PTXFiles/ -outFolder=$1binaryFiles/
cd ../cloudNormals
make
./cloudNormals -inFolder=$1binaryFiles/ -outFolder=$1cloudNormals/
cd ../getRotations
make
./getRotations -inFolder=$1cloudNormals/ -outFolder=$1densityMaps/rotations/
cd ../scanDensity
make
./scanDensity -dataPath=$1 -scale=98.0
cd ../placeScan
make
./placeScan -dataPath=$1 -nopreviewOut -redo -V1