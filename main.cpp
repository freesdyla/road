#include "VisionArmCombo.h"
#include <iostream>
#include <ctime>
#include <math.h>

int main(int argc, char**argv)
{
	VisionArmCombo vac;

#if 0
	while (1) {
		float a;
		float t;
		cout << "input float a" << endl;
		cin >> a;
		cout << "a:  " << a << endl;
		cin >> t;
		cout << "t:  " << t << endl;
		vac.getWater(a, true); //a->weight; true->water; false->additive	

		cout << "enter to next turn" << endl;
		getchar();
	}
#endif

	//vac.calibrateToolCenterPoint(8); std::getchar(); return 0;

	//vac.calibrateGripperTip(4, "tube_2");

	//vac.testLineScannerProbeCalibrationMatrix(); std::getchar(); return 0;
	vac.placePots(PICK_POT);
	//vac.placePots(PLACE_POT);
//	vac.gotoBalance();

//	vac.scanAndProbeTest();	std::getchar(); return 0;
//	vac.gripper_.close();
//	vac.gotoBalance(); std::getchar(); return 0;
	//vac.localizeByScanSpheres(0, 1); std::getchar(); return 0;

	/*int i = 1;
	while(true)
	{
		std::getchar();
		vac.sendRoboteqVar(1, i);
		i++;
		if (i > 3)
		{
			i = 1;
		}
	}*/

	//vac.markerDetection();

	//vac.calibrateKinectRGBCamera(); vac.KinectRGBHandEyeCalibration();

	//vac.mapWorkspaceUsingKinectArm();

	//vac.extractLeafFromPointCloud();

	//vac.acquireLinesOnPlanes();	std::getchar(); return 0;

//	vac.readCloudAndPoseFromFile(); vac.lineScannerHandEyeCalibration(6);

	//vac.testLineScannerProbeCalibrationMatrix();

	//Eigen::Vector3d v; vac.calibrateToolCenterPoint(v, 6);

	std::getchar();  return 0;
}