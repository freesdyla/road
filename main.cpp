#include "VisionArmCombo.h"
#include <iostream>
#include <ctime>

int main(int argc, char**argv)
{
	VisionArmCombo vac;
	
	//vac.calibrateToolCenterPoint(8); std::getchar(); return 0;

	//vac.calibrateGripperTip(8);

	//vac.testLineScannerProbeCalibrationMatrix(); std::getchar(); return 0;
#if 0
	vac.initEXO_RGB_Cam();
	vac.exo_rgb_cam_->init();


	for (int i = 0; i < 100; i++)
	{
		
		vac.exo_rgb_cam_->acquireRGBImage();

//		std::memcpy(vac.exo_rgb_cam_->rgb_frame.ptr<uchar>(0), vac.exo_rgb_cam_->rgb_buffer_, 1920 * 1200 * 3);
		cv::Mat tmp;
		cv::resize(vac.exo_rgb_cam_->rgb_frame, tmp, cv::Size(), 0.5, 0.5);
		cv::imshow("exo", tmp);
		cv::waitKey(0);
	}
#endif

//	vac.scanAndProbeTest();	std::getchar(); return 0;
//	vac.gripper_.close();
//	vac.gotoBalance(); std::getchar(); return 0;
	vac.localizeByScanSpheres(0, 1); std::getchar(); return 0;

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