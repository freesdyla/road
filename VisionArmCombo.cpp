#include "VisionArmCombo.h"

VisionArmCombo::VisionArmCombo() :
	voxel_grid_size_(0.002f),
	counter_(0)
{
	std::ifstream input("pot_labels.txt");

	if (input.is_open())
	{
		int line_num = 0;
		for (std::string line; std::getline(input, line); line_num++)
		{
			if (line_num == 0) continue;
			//std::cout << line << std::endl;

			boost::replace_all(line, ",", "_");

			pot_labels.push_back(line);

			std::cout << line << "\n";
		}

		std::cout << "pot num: " << pot_labels.size() << "\n";
	}
	else
	{
		std::cout << "cannot open file\n";
	}

	initVisionCombo();
}

VisionArmCombo::~VisionArmCombo()
{
	//motor_controller_.Disconnect();
}

void VisionArmCombo::initVisionCombo()
{
	initPotMap();
	initRobotArmClient();
	initLineProfiler();

#if 1
	gripper_.activate();
	gripper_.open();
	//gripper_.close();
#endif

	viewer_.reset(new pcl::visualization::PCLVisualizer("3D Viewer"));
	viewer_->addCoordinateSystem(0.3);
	viewer_->setSize(800, 600);
	viewer_->setPosition(0, 0);
	viewer_->registerPointPickingCallback(&VisionArmCombo::pp_callback, *this);

	// load line scanner hand eye calibration matrix
	guessHandToScanner_ = Eigen::Matrix4f::Identity();
	guessHandToScanner_.block<3, 3>(0, 0) = Eigen::AngleAxisf(-0.5*M_PI, Eigen::Vector3f::UnitZ()).matrix();
	guessHandToScanner_.col(3).head(3) << 0.076, 0.0, 0.094;

	handToScanner_ = guessHandToScanner_;

	//std::cout << "hand to scanner\n" << handToScanner_ << "\n";

	std::ifstream file("lineScannerHandEyeCalibration.bin", std::ios::in | std::ios::binary);
	if (file.is_open())
	{
		char buffer[64];
		file.read(buffer, 64);
		for (int i = 0; i < 4; i++)
			for (int j = 0; j < 4; j++)
			{
				float* ptr = (float*)buffer + i * 4 + j;
				handToScanner_.row(i)(j) = *(ptr);
			}
		std::cout << "handToScanner:\n" << handToScanner_ << "\n";
	}
	else std::cout << "lineScannerHandEyeCalibration load fail\n";
	file.close();

	cv::FileStorage fs("gripper_tip_calib.yml", cv::FileStorage::READ);
	cv::Vec3d tcp;
	cv::Vec3d tcp_tube_1, tcp_tube_2;
	if (fs.isOpened())
	{
		fs["tcp"] >> tcp;
		fs["tube_1"] >> tcp_tube_1;
		fs["tube_2"] >> tcp_tube_2;
	}
	fs.release();

	for (int i = 0; i < 3; i++) tool_center_point_(i) = tcp[i];

	//tool_center_point_ << -0.06905, 0.15891, 0.14141;	//ROAD, laser pointer, CAD model

	//tool_center_point_ << 0.0348893, -0.0440583, 0.18337;	//10/24/2016

	//std::cout << "tcp: " << tcp<<"\n";

	probe_to_hand_ = Eigen::Matrix4d::Identity();

	probe_to_hand_.col(3).head<3>() = tool_center_point_.cast<double>();

	probe_to_hand_ = probe_to_hand_.inverse();

	marker_length_ = 0.1016f;	//4 inch

	hand_to_gripper_ = Eigen::Matrix4d::Identity();

	//hand_to_gripper_.topLeftCorner<3,3>() = Eigen::AngleAxisd(M_PI, Eigen::Vector3d::UnitY()).matrix();
	hand_to_gripper_(0, 0) = -1.;
	hand_to_gripper_(2, 2) = -1.;
	
	hand_to_gripper_.col(3).head(3) << 0.0381, -0.0827999, -0.3683;

//	hand_to_gripper_.col(3).head(3) << 0.0, -0.0, -0.3683;

	hand_to_tube1_ = Eigen::Matrix4d::Identity();

	hand_to_tube1_(0, 0) = -1.;
	hand_to_tube1_(2, 2) = -1.;

	hand_to_tube1_.col(3).head(3) << tcp_tube_1[0], tcp_tube_1[1], tcp_tube_1[2];

	fs.open("kinectRGBCalibration.yml", cv::FileStorage::READ);

	if (fs.isOpened())
	{
		fs["camera_matrix"] >> kinect_rgb_camera_matrix_cv_;

		fs["distortion_coefficients"] >> kinect_rgb_dist_coeffs_cv_;

		fs.release();
	}

	fs.open("kinectRGBHandEyeCalibration.yml", cv::FileStorage::READ);

	if (fs.isOpened())
	{
		fs["hand to eye"] >> kinect_rgb_hand_to_eye_cv_;

		fs.release();
	}

	cvTransformToEigenTransform(kinect_rgb_hand_to_eye_cv_, hand_to_rgb_);

#if 0
	marker_dictionary_ = cv::aruco::getPredefinedDictionary(cv::aruco::DICT_5X5_50);

	detector_params_ = cv::aruco::DetectorParameters::create();

	detector_params_->doCornerRefinement = true; // do corner refinement in markers
#endif

	cur_rgb_to_marker_ = Eigen::Matrix4d::Identity();

	pre_point_ << 0, 0, 0;

	scan_start_to_hand_ = Eigen::Matrix4d::Identity();

	scan_start_to_hand_(2, 3) = 0.3; scan_start_to_hand_(1, 3) = -0.05;

	scan_start_to_hand_ = scan_start_to_hand_.inverse()*handToScanner_.cast<double>().inverse();

	scan_end_to_hand_ = Eigen::Matrix4d::Identity();

	scan_end_to_hand_(2, 3) = 0.3; scan_end_to_hand_(1, 3) = +0.05;

	scan_end_to_hand_ = scan_end_to_hand_.inverse()*handToScanner_.cast<double>().inverse();

	kinect_cloud_.reset(new PointCloudT);
	kinect_cloud_->is_dense = true;

	laser_cloud_.reset(new PointCloudT);
	laser_cloud_->is_dense = true;

	// kinect
	cam2hand_kinect_ = Eigen::Matrix4f::Identity();
	cam2hand_kinect_(0, 3) = 0.0540247f;
	cam2hand_kinect_(1, 3) = 0.1026325f;
	cam2hand_kinect_(2, 3) = 0.0825227f;

	ArmConfig imaging_config;
	/*imaging_config.setJointPos(-94.64, -11.94, -134.11, -79.05, 93.28, -183.29);
	imaging_config.toRad();
	imaging_config_vec.push_back(imaging_config);*/

	imaging_config.setJointPos(-87.72, -92.01, -106.85, -71.06, 90.44, -174.31);
	imaging_config.toRad();
	imaging_config_vec.push_back(imaging_config);

	/*imaging_config.setJointPos(-95.44, -84.73, -65.48, -120.08, 90.85, -183.29);
	imaging_config.toRad();
	imaging_config_vec.push_back(imaging_config);*/

	//imaging_config.setJointPos(-95.29, -94.71, -33.16, -136.00, 90.84, -183.12);
	//imaging_config.toRad();
	//imaging_config_vec.push_back(imaging_config);

	//imaging_config.setJointPos(-59.32, -95.62, -31.90, -137.05, 87.10, -147.33);
	//imaging_config.toRad();
	//imaging_config_vec.push_back(imaging_config);

	sor_.setMeanK(50);
	sor_.setStddevMulThresh(1.0);

	fs.open("road_parameters.yml", cv::FileStorage::READ);

	if (fs.isOpened())
	{
		fs["move_arm_speed_"] >> move_arm_speed_;
		fs["move_arm_acceleration_"] >> move_arm_acceleration_;
		fs["move_joint_speed_"] >> move_joint_speed_;
		fs["move_joint_acceleration_"] >> move_joint_acceleration_;
		fs["view_time_"] >> view_time_;
		fs["exposure_time_"] >> exposure_time_;

		fs.release();
	}

#if 1
	initEXO_RGB_Cam();
#endif

	pump = new Motor();

	int status = motor_controller_.Connect("COM1");

	if (status != RQ_SUCCESS)
		std::cout << "Error connecting to motor controller: " << status << "\n";
}

void VisionArmCombo::pp_callback(const pcl::visualization::PointPickingEvent& event, void*)
{
	if (event.getPointIndex() == -1)
		return;
	Eigen::Vector3f current_point;
	event.getPoint(current_point[0], current_point[1], current_point[2]);

	pre_point_idx_ = event.getPointIndex();

	std::cout << "current point:\n" << current_point << "\n";
	std::cout << "distance: " << (current_point - pre_point_).norm() << "\n";
	pre_point_ = current_point;
	pre_viewer_pose_ = viewer_->getViewerPose(0);
	std::cout << "viewer pose\n"<< pre_viewer_pose_.matrix() << "\n";
}

double VisionArmCombo::magnitudeVec3(double * vec)
{
	return sqrt(vec[0] * vec[0] + vec[1] * vec[1] + vec[2] * vec[2]);
}

void VisionArmCombo::array6ToEigenMat4d(double* array6, Eigen::Matrix4d & mat4d)
{
	mat4d = Eigen::Matrix4d::Identity();
	double angle = magnitudeVec3(array6 + 3);
	Eigen::Vector3d axis(array6[3] / angle, array6[4] / angle, array6[5] / angle);
	Eigen::Matrix3d rotationMatrix;
	rotationMatrix = Eigen::AngleAxisd(angle, axis);
	mat4d.block<3, 3>(0, 0) = rotationMatrix;
	mat4d(0, 3) = array6[0]; mat4d(1, 3) = array6[1]; mat4d(2, 3) = array6[2];
}

void VisionArmCombo::eigenMat4dToArray6(Eigen::Matrix4d & mat4d, double * array6)
{
	Eigen::AngleAxisd angle_axis(mat4d.topLeftCorner<3,3>());

	array6[0] = mat4d(0, 3); array6[1] = mat4d(1, 3); array6[2] = mat4d(2, 3);

	Eigen::Vector3d rotation_vector = angle_axis.angle()*angle_axis.axis();

	array6[3] = rotation_vector[0]; array6[4] = rotation_vector[1]; array6[5] = rotation_vector[2];
}

void VisionArmCombo::array6ToEigenMat4(double* array6, Eigen::Matrix4f & mat4)
{
	mat4 = Eigen::Matrix4f::Identity();
	double angle = magnitudeVec3(array6 + 3);
	Eigen::Vector3f axis(array6[3] / angle, array6[4] / angle, array6[5] / angle);
	Eigen::Matrix3f rotationMatrix;
	rotationMatrix = Eigen::AngleAxisf(angle, axis);
	mat4.block<3, 3>(0, 0) = rotationMatrix;
	mat4(0, 3) = array6[0]; mat4(1, 3) = array6[1]; mat4(2, 3) = array6[2];
}


void VisionArmCombo::initRobotArmClient()
{
	robot_arm_client_ = new RobotArmClient();
}

void VisionArmCombo::initLineProfiler()
{
	line_profiler_ = new KeyenceLineProfiler();
}

void VisionArmCombo::initKinectThread()
{
	kinect_thread_ = new KinectThread();
}

void VisionArmCombo::calibrateToolCenterPoint(int numPoseNeeded)
{
	if (robot_arm_client_ == NULL) initRobotArmClient(); 

	int poseIdx = 0;

	std::vector<double*> poseVec;
	poseVec.resize(numPoseNeeded);

	for (int i = 0; i < numPoseNeeded; i++)
	{
		poseVec[i] = new double[6];
	}

	if (numPoseNeeded % 2 != 0 || numPoseNeeded < 4)
	{
		std::cout << "Num of poses needed wrong" << std::endl;
		return;
	}

	Eigen::Vector3d vec3d;

	while (true)
	{
		std::cout << "Press Enter to save pose " << poseIdx << std::endl;
		std::getchar();

		robot_arm_client_->getCartesianInfo(poseVec[poseIdx]);
		robot_arm_client_->printCartesianInfo(poseVec[poseIdx]);

		if (poseIdx == numPoseNeeded - 1)
		{
			//std::cout << "size: "<<poseVec.size() << std::endl;
			Eigen::MatrixXd A(3 * (numPoseNeeded)*(numPoseNeeded-1), 3);
			Eigen::VectorXd b(3 * (numPoseNeeded)*(numPoseNeeded-1));

			int idx = 0;

			for (int i = 0; i < numPoseNeeded; i++)
			{
				for (int j = 0; j < numPoseNeeded; j++)
				{
					if (i != j)
					{
						Eigen::Matrix4d T0;

						array6ToEigenMat4d(poseVec[i], T0);

						//std::cout << "T0" << std::endl << T0 << std::endl;

						Eigen::Matrix4d T1;

						array6ToEigenMat4d(poseVec[j], T1);

						//std::cout << "T1" << std::endl << T1 << std::endl;

						T0 = T0 - T1;

						//std::cout << "T0-T1" << std::endl << T0 << std::endl;

						A.block<3, 3>(3 *idx, 0) = T0.block<3, 3>(0, 0);

						b.block<3, 1>(3 *idx, 0) = T0.block<3, 1>(0, 3);
						++idx;
					}
				}
			}

		/*	Eigen::MatrixXd A(3 * numPoseNeeded / 2, 3);
			Eigen::VectorXd b(3 * numPoseNeeded / 2);

			for (int i = 0; i < numPoseNeeded; i += 2)
			{
				Eigen::Matrix4d T0;

				array6ToEigenMat4d(poseVec[i], T0);

				std::cout << "T0" << std::endl << T0 << std::endl;

				Eigen::Matrix4d T1;

				array6ToEigenMat4d(poseVec[i + 1], T1);

				std::cout << "T1" << std::endl << T1 << std::endl;

				T0 = T0 - T1;

				std::cout << "T0-T1" << std::endl << T0 << std::endl;

				A.block<3, 3>(3 * (i / 2), 0) = T0.block<3, 3>(0, 0);
				b.block<3, 1>(3 * (i / 2), 0) = T0.block<3, 1>(0, 3);
			}*/

			// Solve Ax=b
			//std::cout << "A:" << std::endl << A << std::endl << "b:" << std::endl << b << std::endl;

			vec3d = A.jacobiSvd(Eigen::ComputeThinU | Eigen::ComputeThinV).solve(b);

			vec3d *= -1.;

			std::cout << "x (hand to TCP):" << std::endl << vec3d << std::endl;

			break;
		}

		poseIdx++;
	}

	cv::FileStorage fs("tool_center_point_calib.yml", cv::FileStorage::WRITE);

	cv::Vec3d tcp;
	tcp[0] = vec3d(0); tcp[1] = vec3d(1); tcp[2] = vec3d(2);

	fs << "tcp" << tcp;

	fs.release();
	
	std::cout << "Saved" << std::endl;
}

void VisionArmCombo::calibrateGripperTip(int numPoseNeeded = 4, std::string type = "gripper")
{
	if (robot_arm_client_ == NULL) initRobotArmClient();

	int poseIdx = 0;

	std::vector<double*> poseVec;
	poseVec.resize(numPoseNeeded);

	for (int i = 0; i < numPoseNeeded; i++)
	{
		poseVec[i] = new double[6];
	}

	if (numPoseNeeded % 2 != 0 || numPoseNeeded < 4)
	{
		std::cout << "Num of poses needed wrong" << std::endl;
		return;
	}

	Eigen::Vector3d vec3d;

	while (true)
	{
		std::cout << "Press Enter to save pose " << poseIdx << std::endl;
		std::getchar();

		robot_arm_client_->getCartesianInfo(poseVec[poseIdx]);
		robot_arm_client_->printCartesianInfo(poseVec[poseIdx]);

		if (poseIdx == numPoseNeeded - 1)
		{
			//std::cout << "size: "<<poseVec.size() << std::endl;
			Eigen::MatrixXd A(3 * (numPoseNeeded)*(numPoseNeeded - 1), 3);
			Eigen::VectorXd b(3 * (numPoseNeeded)*(numPoseNeeded - 1));

			int idx = 0;

			for (int i = 0; i < numPoseNeeded; i++)
			{
				for (int j = 0; j < numPoseNeeded; j++)
				{
					if (i != j)
					{
						Eigen::Matrix4d T0;

						array6ToEigenMat4d(poseVec[i], T0);

						Eigen::Matrix4d T1;

						array6ToEigenMat4d(poseVec[j], T1);

						T0 = T0 - T1;

						A.block<3, 3>(3 * idx, 0) = T0.block<3, 3>(0, 0);

						b.block<3, 1>(3 * idx, 0) = T0.block<3, 1>(0, 3);
						++idx;
					}
				}
			}

			vec3d = A.jacobiSvd(Eigen::ComputeThinU | Eigen::ComputeThinV).solve(b);

			vec3d *= -1.;

			std::cout << "x (hand to TCP):" << std::endl << vec3d << std::endl;

			break;
		}

		poseIdx++;
	}

	cv::FileStorage fs_read("gripper_tip_calib.yml", cv::FileStorage::READ);

	std::vector<cv::Vec3d> tcp3(3);

	fs_read["gripper"] >> tcp3[0];
	fs_read["tube_1"] >> tcp3[1];
	fs_read["tube_2"] >> tcp3[2];

	fs_read.release();

	cv::FileStorage fs("gripper_tip_calib.yml", cv::FileStorage::WRITE);

	cv::Vec3d tcp;
	tcp[0] = vec3d(0); tcp[1] = vec3d(1); tcp[2] = vec3d(2);

	if (type == "gripper")
		tcp3[0] = tcp;
	else if (type == "tube_1")
		tcp3[1] = tcp;
	else if (type == "tube_2")
		tcp3[2] = tcp;

	fs << "gripper" << tcp3[0];
	fs << "tube_1" << tcp3[1];
	fs << "tube_2" << tcp3[2];

	fs.release();

	std::cout << "Saved" << std::endl;
}

/*
	assume robot arm already at start pose
	vec3d: motion vector in base frame
	cloud: PCL point cloud to save the data
*/
void VisionArmCombo::scanTranslateOnly(double * vec3d, PointCloudT::Ptr  cloud, float acceleration, float speed)
{
	if (acceleration == 0 || speed == 0)
	{
		std::cout << "acceleration or speed = 0\n";
		return;
	}

	if (robot_arm_client_ == NULL || line_profiler_ == NULL)
	{
		std::cout << "robot arm or line profiler not initialized \n";
		return;
	}

	double curPoseD[6];
	robot_arm_client_->getCartesianInfo(curPoseD);
	//robot_arm_client_->printCartesianInfo(curPoseD);

	double endPoseD[6];
	std::memcpy(endPoseD, curPoseD, 48);
	for (int i = 0; i < 3; i++) endPoseD[i] += vec3d[i];

	//double sync_pose[6];

	if (line_profiler_->device_initialized == false)
		line_profiler_->init();

	line_profiler_->m_vecProfileData.clear();
	
	//double tcp_sync_speed[6];
	robot_arm_client_->setStartPoseXYZ();

	unsigned int start_frame_id = 0;

	robot_arm_client_->moveHandL(endPoseD, acceleration, speed);

	line_profiler_->start(20);

	robot_arm_client_->waitTillTCPMove();

	start_frame_id = line_profiler_->frame_counter_;
	
	//old synchronization
	//line_profiler_->start(20);
	//robot_arm_client_->getCartesianInfo(sync_pose);
	//robot_arm_client_->getTCPSpeed(tcp_sync_speed);
	
	robot_arm_client_->waitTillHandReachDstPose(endPoseD);
	line_profiler_->stop();

	std::cout << "start_frame_id: " << start_frame_id << "\n";

	// register point cloud
	cloud->clear();

	int num_profiles = line_profiler_->m_vecProfileData.size();

	double sync_speed = sqrt(robot_arm_client_->tcp_sync_speed_[0] * robot_arm_client_->tcp_sync_speed_[0] 
					+ robot_arm_client_->tcp_sync_speed_[1] * robot_arm_client_->tcp_sync_speed_[1] 
					+ robot_arm_client_->tcp_sync_speed_[2] * robot_arm_client_->tcp_sync_speed_[2]);

	std::cout <<"tcp sync speed: "<<sync_speed<< "\n";

	std::cout << "num profiles: " << num_profiles << std::endl;

	Eigen::Matrix4f startPose;
	Eigen::Matrix4f endPose;

	array6ToEigenMat4(curPoseD, startPose);
	array6ToEigenMat4(endPoseD, endPose);

	double sync_distance = robot_arm_client_->EuclideanDistance(curPoseD, robot_arm_client_->sync_pose_);

	std::cout << "sync distance: " << sync_distance << "\n";

	//std::cout << "calculated end pose\n" << endPose << "\n\n";

	/*double testPoseD[6];
	robot_arm_client_->getCartesianInfo(testPoseD);
	Eigen::Matrix4f testPose;
	array6ToEigenMat4(testPoseD, testPose);

	std::cout << "true end pose\n" << testPose << "\n\n";*/


	// express motion vector in base frame
	Eigen::Vector3f motionVector( endPoseD[0] - curPoseD[0],
							      endPoseD[1] - curPoseD[1],
								  endPoseD[2] - curPoseD[2] );

	//std::cout << "motion vector in base frame:" << motionVector << std::endl;

	// transform motion vector in scanner frame
	/*Eigen::Matrix3f m;
	m = startPose.block<3, 3>(0,0);
	// A'*B' = (BA)' where A and B are rotation matrices. vec_senosr = Te2s*Tb2e*vec_base
	m = m*Eigen::AngleAxisf(0.5*M_PI, Eigen::Vector3f::UnitZ());
	motionVector = m.transpose()*motionVector;*/

	motionVector = (handToScanner_.block<3,3>(0,0)).transpose()*(startPose.block<3, 3>(0, 0)).transpose()*motionVector;

	//std::cout << "motion vector in sensor frame:" << motionVector << std::endl;

	float magnitude = motionVector.norm();

	Eigen::Vector3f motionDelta = motionVector / (num_profiles - 1);

	//std::cout << "motion delta:" << motionDelta << std::endl;

	//std::cout << "x start: " << line_profiler_->m_profileInfo.lXStart << " pitch: " << line_profiler_->m_profileInfo.lXPitch << "\n";

	// 2k sampling frequency (0.5ms)
	float distance = 0;

	double time = 0;

	time = sync_speed / acceleration;

	std::cout << "start time:" << time << "\n";

	time = sqrt(sync_distance * 2 / acceleration);

	std::cout << "start time from dist: " << time << "\n";

	const float start_cruise_time = speed/acceleration;

	const float stop_cruise_time = start_cruise_time + (magnitude -start_cruise_time*start_cruise_time*acceleration)/speed;

	const float cruise_time = stop_cruise_time - start_cruise_time;

	const float total_time = start_cruise_time + stop_cruise_time;

	const float acceleration_total_distance = 0.5f*acceleration*start_cruise_time*start_cruise_time;
	
	motionVector.normalize();

	for (int i = start_frame_id; i < num_profiles; i++)
	{
		if (time <= start_cruise_time)
			distance = 0.5f*acceleration*time*time;
		else if (time <= stop_cruise_time)
			distance = acceleration_total_distance + (time - start_cruise_time)*speed;
		else
			distance = magnitude - pow(total_time - time, 2.f)*0.5f*acceleration;

		time += 5e-4;	// 0.5 ms

		// the offset maybe be related to scan speed
		Eigen::Vector3f displacement = motionVector*(distance + speed_correction_);

		for (int j = 10; j < 790; j++)
		{
			PointT point;

			point.z = line_profiler_->m_vecProfileData[i].m_pnProfileData[j] * (-1e-8f);

			if (abs(point.z) < 0.140f)
			{
				point.y = displacement(1);
				point.x = (float)(line_profiler_->m_profileInfo.lXStart + j*line_profiler_->m_profileInfo.lXPitch)*(1e-8f) + displacement(0);
				point.z += 0.3f + displacement(2);

				uint32_t r = ((uint32_t)(point.z * 100000)) % 255;
				
				uint32_t rgb = r << 16 | r << 8 | (uint32_t)255;

				point.rgb = *reinterpret_cast<float*>(&rgb);

				//point.r = ((int)(point.z*100000))%255;//(uint8_t)(255.f - 255.f*(point.z + 0.141f) / 0.282f);
				//point.g = point.r;
				//point.b = 255;

				cloud->push_back(point);
			}
		}
	}

	std::cout << "point cloud size: " << cloud->size() << std::endl;
}

void VisionArmCombo::scanLine(PointCloudT::Ptr & cloud)
{
	if (robot_arm_client_ == NULL || line_profiler_ == NULL)
	{
		std::cout << "robot arm or line profiler not initialized \n";
		return;
	}

	if (line_profiler_->device_initialized == false)
		line_profiler_->init();

	double curPoseD[6];
	robot_arm_client_->getCartesianInfo(curPoseD);
	robot_arm_client_->printCartesianInfo(curPoseD);

	//!before start scan,  must clear old data
	line_profiler_->m_vecProfileData.clear();
	line_profiler_->start(10);
	line_profiler_->stop();

	cloud->clear();

	int num_profiles = line_profiler_->m_vecProfileData.size();

	std::cout << "num profiles: " << num_profiles << std::endl;

	//for (int i = 0; i < num_profiles; i++)
	int i = num_profiles / 2;
	{
		for (int j = 10; j < 790; j++)
		{
			PointT point;

			point.z = line_profiler_->m_vecProfileData[i].m_pnProfileData[j] * (-1e-8f);

			if (abs(point.z) < 0.140f)
			{
				point.z += 0.3f;
				point.y = 0.f;
				point.x = (float)(line_profiler_->m_profileInfo.lXStart + j*line_profiler_->m_profileInfo.lXPitch)*(1e-8f);

				point.r = (uint8_t)(255.f - 255.f*(point.z + 0.14f) / 0.28f);
				point.g = point.r;
				point.b = 255;

				cloud->push_back(point);
			}
		}
	}

	std::cout << "point cloud size: " << cloud->size() << std::endl;
}


void VisionArmCombo::readCloudAndPoseFromFile()
{
	std::ifstream file("ScannerRobotArmCalibrationFile.txt");
	
	if (file.is_open())
	{
		std::string line;

		calibration_point_cloud_vec.clear();

		while (std::getline(file, line))
		{
			std::size_t found = line.find_first_of(",");

			std::string front_part;

			double pose[6];

			if (found != std::string::npos && found > 0)
			{
				front_part = line.substr(0, found);
				//std::cout << "file name: " << front_part << "\n";
				
				// trim line
				line = line.substr(found + 1, line.size()-1);

				// read point cloud
				PointCloudT::Ptr tmp_point_cloud (new PointCloudT);
				if(pcl::io::loadPCDFile(front_part, *tmp_point_cloud) == 0)
					calibration_point_cloud_vec.push_back(tmp_point_cloud);
				else
					std::cout<<"load pcd file "<<front_part<<" fail\n";
				
				//std::cout << line << "\n";	

				for (int i = 0; i < 5; i++)
				{
					found = line.find_first_of(",");

					if (found != std::string::npos)
					{
						front_part = line.substr(0, found);
						pose[i] = std::stod(front_part, 0);
						//std::cout << i << " " << pose[i] << "\n";
						line = line.substr(found + 1, line.size() - 1);
					}
					else
					{
						std::cout << "wrong line\n";
						break;
					}
				}

				pose[5] = std::stod(line, 0);
				//std::cout << "5 " << pose[5] << "\n";

				Eigen::Matrix4d *tmp_pose = new Eigen::Matrix4d;
				array6ToEigenMat4d(pose, *tmp_pose);
				hand_pose_vec_.push_back(tmp_pose);
			}
			else
			{
				std::cout << "wrong line\n";
				break;
			}				
		}

		file.close();
	}
	else std::cout << "Unable to open file";
}

std::string VisionArmCombo::getCurrentDateTimeStr()
{
	SYSTEMTIME st;

	GetSystemTime(&st);

	char currentTime[84] = "";

	std::sprintf(currentTime, "%d-%d-%d-%d-%d-%d-%d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

	return std::string(currentTime);
}




/*
	register point cloud from kinect in robot base frame
*/

void VisionArmCombo::mapWorkspaceUsingKinectArm()
{
	//pcl::io::loadPCDFile("laser_scan.pcd", *kinect_cloud_);

	//extractLeafProbingPoints(kinect_cloud_);

	//return;

	//pp_.PRMCEPreprocessing(); pp_.savePathPlanner("pp"); for (int i=1; i<pp_.num_nodes_; i+=2) viewPlannedPath(pp_.random_nodes_buffer_+(i-1)*6, pp_.random_nodes_buffer_+i*6); return;
	initRobotArmClient();
	initKinectThread();
	initLineProfiler();

	int scan_count = 0;

	if (!pp_.loadPathPlanner("pp"))
	{
		std::cout << "load path planner fail\n";
		return;
	}

	std::cout << "acquire Kinect data...\n";
	for (int i = 0; i < imaging_config_vec.size(); i++)
	{
		moveToConfigGetKinectPointCloud(imaging_config_vec[i], true, true);
	}

	// no data
	if (kinect_cloud_->points.size() < 100) return;

	processGrowthChamberEnviroment(kinect_cloud_, -0.6f, 1);

	return;

	//pcl::io::savePCDFileBinary("supervoxel.pcd", *kinect_cloud_);

	// Normal estimation
	std::cout << "Normal estimation\n";
	// Create the normal estimation class, and pass the input dataset to it
	pcl::NormalEstimation<PointT, pcl::Normal> ne;
	ne.setInputCloud(kinect_cloud_);

	// Create an empty kdtree representation, and pass it to the normal estimation object.
	// Its content will be filled inside the object, based on the given input dataset (as no other search surface is given).
	pcl::search::KdTree<PointT>::Ptr tree(new pcl::search::KdTree<PointT>());
	ne.setSearchMethod(tree);

	// Output datasets
	pcl::PointCloud<pcl::Normal>::Ptr cloud_normals(new pcl::PointCloud<pcl::Normal>);

	// Use all neighbors in a sphere of radius 3cm
	ne.setRadiusSearch(0.02);

	// Compute the features
	ne.compute(*cloud_normals);

	viewer_->removeAllPointClouds();
	viewer_->removeAllShapes();

	std::vector<pcl::Supervoxel<PointT>::Ptr> potential_probe_supervoxels;

	extractProbeSurfacePatchFromPointCloud(kinect_cloud_, potential_probe_supervoxels);

	std::cout << "potential_probe_supervoxels size: " << potential_probe_supervoxels.size() << "\n";

	for (int i = 0; i < potential_probe_supervoxels.size(); i++)
	{
		if (abs(potential_probe_supervoxels[i]->normal_.normal_z) < 0.8)
			continue;

		std::cout << "potential supervoxel " << i << "\n";

		PointT point;
		point.x = potential_probe_supervoxels[i]->centroid_.x;
		point.y = potential_probe_supervoxels[i]->centroid_.y;
		point.z = potential_probe_supervoxels[i]->centroid_.z;

		std::vector<ArmConfig> solution_config_vec;
		Eigen::Matrix4d scan_start_hand_pose;
		Eigen::Vector3d hand_translation;
		
		// compute laser scan pose
		if (computeCollisionFreeProbeOrScanPose(point, potential_probe_supervoxels[i]->normal_, false, solution_config_vec, scan_start_hand_pose, hand_translation))
		{
			//std::cout << "hand translation: " << hand_translation.transpose() << "\n";

			// move to the start scan pose
			if (moveToConfigGetKinectPointCloud(solution_config_vec[0], false, true))
			{
				double vec[3];
				vec[0] = hand_translation(0);
				vec[1] = hand_translation(1);
				vec[2] = hand_translation(2);

				PointCloudT::Ptr scan_cloud(new PointCloudT);

				//wait 1 sec
				Sleep(1000);

				// moveToConfig moves the arm to a pose slightly different from scan_start_hand_pose
				Eigen::Matrix4f curPose; getCurHandPose(curPose);

				//std::cout << curPose << "\n" << scan_start_hand_pose << "\n";

				scan_start_hand_pose = curPose.cast<double>();


				// Use laser scanner
				scanTranslateOnly(vec, scan_cloud, scan_acceleration_, scan_speed_);
				
				PointCloudT::Ptr scan_cloud_down(new PointCloudT);

				vox_.setLeafSize(0.001, 0.001, 0.001);

				vox_.setInputCloud(scan_cloud);

				vox_.filter(*scan_cloud_down);

				sor_.setInputCloud(scan_cloud_down);
				
				PointCloudT::Ptr scan_cloud_down_filter(new PointCloudT);

				sor_.filter(*scan_cloud_down_filter);

				PointCloudT::Ptr scan_cloud_in_base(new PointCloudT);

				pcl::transformPointCloud(*scan_cloud_down_filter, *scan_cloud_in_base, scan_start_hand_pose.cast<float>()*handToScanner_);

				// insert a probe test
				//probeScannedSceneTest(scan_cloud_in_base);


				//std::cout << "supervoxel on scanned point cloud\n";

				std::vector<pcl::Supervoxel<PointT>::Ptr> scanned_supervoxels;

				extractProbeSurfacePatchFromPointCloud(scan_cloud_in_base, scanned_supervoxels);

				pcl::PointCloud<pcl::PointXYZRGBA>::Ptr centroid_cloud(new pcl::PointCloud<pcl::PointXYZRGBA>);

				for (auto ssv : scanned_supervoxels)
				{
					centroid_cloud->push_back(ssv->centroid_);
				}

				pcl::KdTreeFLANN<pcl::PointXYZRGBA> kdtree;

				kdtree.setInputCloud(centroid_cloud);

				int K = 1;
				std::vector<int> pointIdxNKNSearch(K);
				std::vector<float> pointNKNSquaredDistance(K);

				std::vector<pcl::Supervoxel<PointT>::Ptr> filtred_scanned_supervoxels;

				if (kdtree.nearestKSearch(potential_probe_supervoxels[i]->centroid_, K, pointIdxNKNSearch, pointNKNSquaredDistance) > 0)
				{
					for (size_t i = 0; i < pointIdxNKNSearch.size(); ++i)
					{
						filtred_scanned_supervoxels.push_back(scanned_supervoxels[pointIdxNKNSearch[i]]);

						/*	std::cout << "    " << cloud->points[pointIdxNKNSearch[i]].x
								<< " " << cloud->points[pointIdxNKNSearch[i]].y
								<< " " << cloud->points[pointIdxNKNSearch[i]].z
								<< " (squared distance: " << pointNKNSquaredDistance[i] << ")" << std::endl;*/
					}
				}
				else
					continue;

				scanned_supervoxels = filtred_scanned_supervoxels;

				float c2c_dist = 0.f;
				c2c_dist += pow(potential_probe_supervoxels[i]->centroid_.x - scanned_supervoxels[0]->centroid_.x, 2.f);
				c2c_dist += pow(potential_probe_supervoxels[i]->centroid_.y - scanned_supervoxels[0]->centroid_.y, 2.f);
				c2c_dist += pow(potential_probe_supervoxels[i]->centroid_.z - scanned_supervoxels[0]->centroid_.z, 2.f);
				std::cout << "c2c dist: " << c2c_dist << "\n";
				

				//std::cout << "done\n";

				std::cout << "showing scanned scene + Kinect scene\n";
				//viewer_->removeAllPointClouds();
				viewer_->removePointCloud("scan_cloud_in_base", 0);
				viewer_->addPointCloud(scan_cloud_in_base, "scan_cloud_in_base", 0);
				viewer_->addPointCloud(kinect_cloud_, "kinect_cloud_", 0);
				display();
				viewer_->removePointCloud("kinect_cloud_", 0);
				viewer_->addPointCloud(scanned_supervoxels[0]->voxels_, "sv", 0);
				display();

				std::cout << "scanned_supervoxels size: " << scanned_supervoxels.size() << "\n";
				
				for (int j = 0; j < scanned_supervoxels.size(); j++)
				{
					
					pcl::Supervoxel<PointT>::Ptr sv = scanned_supervoxels[j];

					if (abs(sv->normal_.normal_z) < 0.8 || sv->centroid_.z <= -0.16f) continue;

					std::cout << "supervoxel on scanned scene " << j << "\n";
					
					//find the closest point on surface patch to centroid
					std::cout << "find nearest point on surface to centroid\n";
					int optimal_idx = -1;
					float min_dist = 1000.f;

					for (int k = 0; k < sv->voxels_->points.size(); k++)
					{
						// compute distance
						double distance = pow(sv->voxels_->points[k].x - sv->centroid_.x, 2.f);
						distance += pow(sv->voxels_->points[k].y - sv->centroid_.y, 2.f);
						distance += pow(sv->voxels_->points[k].z - sv->centroid_.z, 2.f);

						if (distance < min_dist)
						{
							min_dist = distance;
							optimal_idx = k;
						}
					}

					if (optimal_idx == -1) continue;

					PointT probe_point;
					probe_point.x = sv->voxels_->points[optimal_idx].x;
					probe_point.y = sv->voxels_->points[optimal_idx].y;
					probe_point.z = sv->voxels_->points[optimal_idx].z;

					probe_point.x = sv->centroid_.x;
					probe_point.y = sv->centroid_.y;
					probe_point.z = sv->centroid_.z;

					std::cout << "showing probe point\n";
					pcl::ModelCoefficients sphere; sphere.values.resize(4);
					sphere.values[0] = probe_point.x; sphere.values[1] = probe_point.y; sphere.values[2] = probe_point.z;
					sphere.values[3] = 0.01;

					viewer_->removeShape("probe point", 0);
					viewer_->addSphere(sphere, "probe point", 0);
					display();

					std::cout << "Probe this point?\n";

					char key = std::getchar();

					if (key != 'y')
					{
						std::cout << "skip\n";
						continue;
					}

					std::vector<ArmConfig> solution_config_vec_refine;
					Eigen::Matrix4d probe_hand_pose_final;
					Eigen::Vector3d hand_translation_refine;

					std::cout << "try to probe\n";

					// try probe
					if (computeCollisionFreeProbeOrScanPose(probe_point, sv->normal_, true, solution_config_vec_refine, probe_hand_pose_final, hand_translation_refine))
					{
						
						//pp_.forwardKinematicsUR10(solution_config_vec_refine[0].joint_pos_f);
						// compute the hand pose with probe 10cm away from the target
						Eigen::Matrix4d final_to_prepare = Eigen::Matrix4d::Identity();
						final_to_prepare(2, 3) = -0.1;
						Eigen::Matrix4d probe_hand_pose_prepare = probe_hand_pose_final*probe_to_hand_.inverse()*final_to_prepare*probe_to_hand_;

						//std::cout << "probe_hand_pose_final\n" << probe_hand_pose_final << "\n";

						//std::cout << "probe_hand_pose_prepare\n" << probe_hand_pose_prepare << "\n";

						// visualization
						/*std::cout << "showing prepare probe pose\n";
						Eigen::Affine3f affine_pose;
						affine_pose.matrix() = probe_hand_pose_prepare.cast<float>();
						viewer_->removeAllCoordinateSystems();
						viewer_->addCoordinateSystem(0.2, affine_pose, "prepare probe pose", 0);
						display();*/
					

						// ik
						std::vector<int> ik_sols_vec;

						inverseKinematics(probe_hand_pose_prepare, ik_sols_vec);

						bool solution_found = false;

						float config[6];

						ArmConfig prepare_probe_config;

						for (auto idx : ik_sols_vec)
						{
							float sol_f[6];

							double2float(ik_sols_ + idx * num_joints_, sol_f);

							if (!pp_.collisionCheckForSingleConfig(sol_f))
							{
								double2float(ik_sols_ + idx * num_joints_, config);

								prepare_probe_config.setJointPos(ik_sols_[idx*num_joints_], ik_sols_[idx*num_joints_ + 1],
									ik_sols_[idx*num_joints_ + 2], ik_sols_[idx*num_joints_ + 3],
									ik_sols_[idx*num_joints_ + 4], ik_sols_[idx*num_joints_ + 5]);

								solution_found = true;
							}
						}

						if (!solution_found)
						{
							std::cout << "probe_hand_pose_prepare ik solution not found\n";
							continue;
						}

						std::cout << "try to move to prepare probe pose\n";

						if (!moveToConfigGetKinectPointCloud(prepare_probe_config, false, true))
						{
							std::cout << "could not reach prepare probe pose\n";
							continue;
						}

						std::cout << "reached prepare probe pose\n";

						display();

						std::cout << "move to final probe pose\n";

						robot_arm_client_->moveHandJ(solution_config_vec_refine[0].joint_pos_d, 0.05, 0.05, true);

						std::cout << "done\n";

						/*double cur_pose[6];

						robot_arm_client_->getCartesianInfo(cur_pose);

						Eigen::Matrix4d probe_pose;

						array6ToEigenMat4d(cur_pose, probe_pose);

						probe_pose.col(3).head<3>() = pre_point_.cast<double>();

						probe_pose = probe_pose*probe_to_hand_;

						cur_pose[0] = probe_pose(0, 3);
						cur_pose[1] = probe_pose(1, 3);
						cur_pose[2] = probe_pose(2, 3);

						robot_arm_client_->moveHandL(cur_pose, 0.05, 0.05);*/
					}

				}
			}
		}

		/*pcl::ModelCoefficients sphere; sphere.values.resize(4);
		sphere.values[0] = point.x; sphere.values[1] = point.y; sphere.values[2] = point.z;
		sphere.values[3] = 0.01;

		viewer_->addSphere(sphere, "sphere" + std::to_string(i), 0);
		display();*/
	}

	return;

	while (true)
	{
		std::cout << "\nPick a point\n";
		display();

		pre_normal_ = cloud_normals->points[pre_point_idx_];

		std::cout << "normal: " << pre_normal_ << "\n";

		
		Eigen::Vector3d z_dir;
		
		if(pre_normal_.normal_z > 0)
			z_dir << -pre_normal_.normal_x, -pre_normal_.normal_y, -pre_normal_.normal_z;
		else
			z_dir << pre_normal_.normal_x, pre_normal_.normal_y, pre_normal_.normal_z;

		Eigen::Vector3d neg_y(0., -1., 0.);

		Eigen::Vector3d x_dir = neg_y.cross(z_dir);

		Eigen::Vector3d y_dir = z_dir.cross(x_dir);

		std::cout << "x dir\n" << x_dir << "\n";
		std::cout << "y dir\n" << y_dir << "\n";
		std::cout << "z dir\n" << z_dir << "\n";

		Eigen::Matrix4d pose = Eigen::Matrix4d::Identity();

		//pose.block<3, 3>(0, 0) = pre_viewer_pose_.matrix().cast<double>().block<3, 3>(0, 0);
		pose.block<3, 1>(0, 0) = x_dir; pose.block<3, 1>(0, 1) = y_dir; pose.block<3, 1>(0, 2) = z_dir;

		pose.block<3, 1>(0, 3) = pre_point_.cast<double>();

		// substract tool center point
		pose.block<3, 1>(0, 3) = pre_point_.cast<double>() - pose.block<3, 3>(0, 0)*tool_center_point_.cast<double>();

		std::cout << pose << "\n";

		std::vector<int> ik_sols_vec;
		int num_sol = inverseKinematics(pose, ik_sols_vec);

		for (int i = 0; i < num_sol; i++)
		{
			std::cout << "solution " << i << ": ";
			for (int j = 0; j < 6; j++)
			{
				std::cout << ik_sols_[i * 6 + j]/PI*180. << " ";
			}
			
			std::vector<PathPlanner::RefPoint> ref_points;
			std::vector<Eigen::Matrix3f> rot_mats;
			std::vector<PathPlanner::OBB> arm_obbs;

			float sol_f[6];

			double2float(ik_sols_ + i * 6, sol_f);

			std::cout << "	self collision: " << pp_.selfCollision(sol_f);

			// visualization
			//pp_.computeReferencePointsOnArm(sol_f, ref_points, rot_mats);
			//pp_.getArmOBBModel(ref_points, rot_mats, arm_obbs); addOBBArmModelToViewer(arm_obbs, viewer_);

			std::cout << "\n";
		}

		
		if (ik_sols_vec.size() == 0)
		{
			std::cout << "no ik solution\n";
			continue;
		}
		else
		{
			double* ik_sol_ptr = ik_sols_ + ik_sols_vec[0] * 6;
			ArmConfig ac;
			ac.setJointPos(*ik_sol_ptr, *(ik_sol_ptr + 1), *(ik_sol_ptr + 2), *(ik_sol_ptr + 3), *(ik_sol_ptr + 4), *(ik_sol_ptr + 5));

			moveToConfigGetKinectPointCloud(ac, false, false);
		}
	}

	return;

	robot_arm_client_->stopRecvTCP();
}

/*
	iteractive calibration process
*/
void VisionArmCombo::testLineScannerProbeCalibrationMatrix()
{
	if(robot_arm_client_ == NULL) initRobotArmClient();

	if(line_profiler_ == NULL) initLineProfiler();

	PointCloudT::Ptr cloud(new PointCloudT);

	double pos[6];

	pcl::KdTreeFLANN<PointT> kdtree;

	while (true)
	{
		std::cout << "Move robot hand then press Enter to scan or 'q'+Enter to close\n";

		std::getchar();

		PointCloudT::Ptr tmp_cloud(new PointCloudT);

		scanLine(tmp_cloud);

		
		Eigen::Matrix4f base2hand;
		robot_arm_client_->getCartesianInfo(pos);
		array6ToEigenMat4(pos, base2hand);
		
		Eigen::Matrix4f base2scanner = base2hand*handToScanner_;

		PointCloudT::Ptr tmp_cloud_base(new PointCloudT);
		pcl::transformPointCloud(*tmp_cloud, *tmp_cloud_base, base2scanner);

		*cloud += *tmp_cloud_base;
		
		viewer_->removeAllPointClouds();

		viewer_->addPointCloud(cloud, "cloud", 0);

		display();

		std::cout << "move hand?\n";

		std::string key;
		std::getline(std::cin, key);

		if (key == "y")
		{
			Eigen::Vector3f trans = pre_point_ - base2hand.topLeftCorner<3, 3>()*tool_center_point_;

			pos[0] = trans(0); pos[1] = trans(1); pos[2] = trans(2);

			robot_arm_client_->moveHandL(pos, 0.05, 0.05);
		}
	}

	robot_arm_client_->stopRecvTCP();
	line_profiler_->stop();
	line_profiler_->finalize();
}

void VisionArmCombo::acquireLinesOnPlanes()
{
	if(robot_arm_client_ == NULL) initRobotArmClient();

	if(line_profiler_ == NULL) initLineProfiler();

	int line_count = 0;
	int plane_count = 0;
	int num_lines_per_plane = 6;

	while (true)
	{
		std::cout << "Move robot hand to plane id " <<plane_count<<" then press Enter to scan on the plane or 'q'+Enter to close\n";

		char key = std::getchar();

		if (key == 'q')
			break;

		//std::vector<PointCloudT::Ptr> line_vec;

		for (int i = 0; i < num_lines_per_plane; i++)
		{
			std::cout << "	press Enter to scan line "<<i<<" on plane " <<plane_count<<"\n";

			char key = std::getchar();

			std::cout << "scan line id " << line_count << "\n";

			PointCloudT::Ptr point_cloud(new PointCloudT);

			scanLine(point_cloud);

			// save data
			std::string filename = getCurrentDateTimeStr() + ".pcd";

			pcl::io::savePCDFile(filename, *point_cloud, true);

			result_file_.open("ScannerRobotArmCalibrationFile.txt", std::ios::app);

			double curPoseD[6];

			robot_arm_client_->getCartesianInfo(curPoseD);

			Eigen::Matrix4d mat4d;
			array6ToEigenMat4d(curPoseD, mat4d);
			std::cout << "pose:\n" << mat4d << "\n";

			char buffer[100];

			sprintf(buffer, "%f,%f,%f,%f,%f,%f", curPoseD[0], curPoseD[1], curPoseD[2], curPoseD[3], curPoseD[4], curPoseD[5]);

			std::string startPoseStr(buffer);

			result_file_ << filename << "," << startPoseStr << "," << plane_count << "," << line_count << std::endl;

			result_file_.close();

			viewer_->removeAllPointClouds();
			viewer_->addPointCloud(point_cloud);
			display();

			//line_vec.push_back(point_cloud);

			line_count++;
		}

		plane_count++;
	}

	robot_arm_client_->stopRecvTCP();
	line_profiler_->stop();
	line_profiler_->finalize();
}

/*
Solve scanner pose wrt robot hand assuming normalDistanceVec populated.
Carlson, F. B., Johansson, R., & Robertsson, A. (2015, September).
Six DOF eye-to-hand calibration from 2D measurements using planar constraints.
In Intelligent Robots and Systems (IROS), 2015 IEEE/RSJ International Conference on (pp. 3628-3632). IEEE.
*/
void VisionArmCombo::lineScannerHandEyeCalibration(int num_lines_per_plane)
{
	Eigen::Matrix4d curHandToScanner_ = guessHandToScanner_.cast<double>();

	std::cout << "initial guess:\n" << curHandToScanner_ << "\n";

	int rows = 0; for (auto c : calibration_point_cloud_vec) rows += c->points.size();

	Eigen::MatrixXd A(rows, 9);
	Eigen::VectorXd Y(rows);
	Eigen::VectorXd w(6);
	double min_rms = 1e5;
	std::cout << "A rows: " << rows << "\n";

	for (int iter = 0; iter < 20; iter++)
	{
		std::cout << "\niteration " << iter << "\n";
		double rms = 0.;
		plane_normal_embed_dist_vec.clear();

		// compute plane normal
		for (int i = 0; i < calibration_point_cloud_vec.size(); i += num_lines_per_plane)
		{
			PointCloudT::Ptr plane_cloud(new PointCloudT);

			for (int j = 0; j < num_lines_per_plane; j++)
			{
				Eigen::Matrix4f base_to_scanner;
				base_to_scanner = hand_pose_vec_[i + j]->cast<float>()*curHandToScanner_.cast<float>();
				PointCloudT::Ptr tmp_cloud(new PointCloudT);
				pcl::transformPointCloud(*calibration_point_cloud_vec[i + j], *tmp_cloud, base_to_scanner);
				*plane_cloud += *tmp_cloud;
				/*viewer_->removeAllPointClouds();
				viewer_->addPointCloud(calibration_point_cloud_vec[i + j]);
				display();*/
			}

			//Eigen::Matrix3d covar_mat = Eigen::Matrix3d::Identity();
			//double xmean = 0.; double ymean = 0.; double zmean = 0.;
			////compute mean
			//for (int j = 0; j < plane_cloud->points.size(); j++)
			//{
			//	xmean += plane_cloud->points[j].x;
			//	ymean += plane_cloud->points[j].y;
			//	zmean += plane_cloud->points[j].z;
			//}

			//xmean /= plane_cloud->points.size();
			//ymean /= plane_cloud->points.size();
			//zmean /= plane_cloud->points.size();

			//double xy_covar = 0.; double xz_covar = 0.; double yz_covar = 0.;
			//double xx_covar = 0.; double yy_covar = 0.; double zz_covar = 0.;
			//for (int j = 0; j < plane_cloud->points.size(); j++)
			//{
			//	xy_covar += (plane_cloud->points[j].x - xmean)*(plane_cloud->points[j].y - ymean);
			//	xz_covar += (plane_cloud->points[j].x - xmean)*(plane_cloud->points[j].z - zmean);
			//	yz_covar += (plane_cloud->points[j].z - zmean)*(plane_cloud->points[j].y - ymean);

			//	xx_covar += (plane_cloud->points[j].x - xmean)*(plane_cloud->points[j].x - xmean);
			//	yy_covar += (plane_cloud->points[j].y - ymean)*(plane_cloud->points[j].y - ymean);
			//	zz_covar += (plane_cloud->points[j].z - zmean)*(plane_cloud->points[j].z - zmean);
			//}

			//covar_mat.diagonal() << xx_covar, yy_covar, zz_covar;
			//covar_mat(0, 1) = xy_covar; covar_mat(0, 2) = xz_covar; covar_mat(1, 2) = yz_covar;
			//covar_mat(1, 0) = xy_covar; covar_mat(2, 0) = xz_covar; covar_mat(2, 1) = yz_covar;

			//Eigen::Vector3d centroid_d(xmean, ymean, zmean);

			//Eigen::EigenSolver<Eigen::Matrix3d> es(covar_mat);

			//int min_idx = 0;
			//double min_eigen = es.eigenvalues()[0].real();
			//for (int j = 1; j < 3; j++)
			//{
			//	if (es.eigenvalues()[j].real() < min_eigen)
			//	{
			//		min_idx = j; min_eigen = es.eigenvalues()[j].real();
			//	}
			//}

			//Eigen::Vector3d min_eigen_vector = es.eigenvectors().col(min_idx).real();


			// estimate normal
			pcl::PCA<PointT> pca(*plane_cloud);

			Eigen::Vector4d centroid = pca.getMean().cast<double>();

			//std::cout << "centroid \n" << centroid << "\n";

			Eigen::Vector3d eigen_values = pca.getEigenValues().cast<double>();
			//std::cout << "eigen values \n" << eigen_values << "\n";

			Eigen::Matrix3d eigen_vectors = pca.getEigenVectors().cast<double>();

			//std::cout << "eigen vector \n" << eigen_vectors << "\n";

			// n in the paper
			Eigen::Vector3d* plane_normal_embed_dist = new Eigen::Vector3d;

			(*plane_normal_embed_dist) = eigen_vectors.col(2)*(eigen_vectors.col(2).dot(centroid.head<3>()));
			// try my own eigen decompo
			//(*plane_normal_embed_dist) = min_eigen_vector*(min_eigen_vector.dot(centroid_d));

			plane_normal_embed_dist_vec.push_back(plane_normal_embed_dist);

		/*	pcl::ModelCoefficients line;
			line.values.resize(6);
			line.values[0] = 0.f; line.values[1] = 0.f; line.values[2] = 0.f;
			line.values[3] = (*plane_normal_embed_dist)(0); line.values[4] = (*plane_normal_embed_dist)(1);
			line.values[5] = (*plane_normal_embed_dist)(2);

			viewer_->removeAllShapes();
			std::string name = "line";
			viewer_->addLine(line, name, 0);
			viewer_->removeAllPointClouds();
			viewer_->addPointCloud(plane_cloud);
			display();*/
		}

		// compute A and Y
		int row_idx = 0; rms = 0.;
		for (int i = 0; i < calibration_point_cloud_vec.size(); i += num_lines_per_plane)
		{
			Eigen::Vector3d n = *plane_normal_embed_dist_vec[i / num_lines_per_plane];

			for (int j = 0; j < num_lines_per_plane; j++)
			{
				PointCloudT::Ptr cloud = calibration_point_cloud_vec[i + j];

				Eigen::RowVector3d nR = n.transpose()*hand_pose_vec_[i + j]->topLeftCorner<3, 3>();

				double np = n.dot(hand_pose_vec_[i + j]->topRightCorner<3,1>());

				for (auto & ps : cloud->points)
				{
					A.block<1, 3>(row_idx, 0) = nR*(double)ps.x;
					A.block<1, 3>(row_idx, 3) = nR*(double)ps.z;
					A.block<1, 3>(row_idx, 6) = nR;
					Y(row_idx++) = n.dot(n) - np;
				}
			}
		}

		w.head<3>() = curHandToScanner_.topLeftCorner<3, 1>(); w.segment<3>(3) = curHandToScanner_.col(2).head(3);
		w.tail<3>() = curHandToScanner_.col(3).head(3);
		rms = sqrt((A*w - Y).squaredNorm() / rows);
		std::cout << "Before RMS: " <<  rms << "\n";

		// solve w using SVD
		w = A.jacobiSvd(Eigen::ComputeThinU | Eigen::ComputeThinV).solve(Y);
		
		std::cout << "w:\n" << w.transpose() << "\n";

		Eigen::Matrix3d tmp_rot;
		Eigen::Vector3d rx = w.head<3>();
		Eigen::Vector3d rz = w.segment<3>(3);
		tmp_rot.col(0) = rx;
		tmp_rot.col(1) = rz.cross(rx);
		tmp_rot.col(2) = rz;

		// Orthogonalize rotational matrix
		Eigen::JacobiSVD<Eigen::Matrix3d> svd(tmp_rot, Eigen::ComputeThinU | Eigen::ComputeThinV);
		Eigen::Matrix3d U = svd.matrixU();
		Eigen::Matrix3d V = svd.matrixV().transpose();
		Eigen::Matrix3d tmp = Eigen::Matrix3d::Identity();
		tmp(2, 2) = (U*V).determinant();
		Eigen::Matrix3d rot_hand_to_sensor = U*tmp*V;

		Eigen::MatrixXd A16 = A.topLeftCorner(rows, 6);
		Eigen::MatrixXd A79 = A.topRightCorner(rows, 3);

		Eigen::VectorXd R_tilde_star(6);
		R_tilde_star.head<3>() = rot_hand_to_sensor.col(0);
		R_tilde_star.tail<3>() = rot_hand_to_sensor.col(2);
		
		Eigen::VectorXd Y_tilde = Y - A16*R_tilde_star;

		Eigen::Vector3d tran_hand_to_sensor = A79.jacobiSvd(Eigen::ComputeThinU | Eigen::ComputeThinV).solve(Y_tilde);

		curHandToScanner_ = Eigen::Matrix4d::Identity();
		curHandToScanner_.topLeftCorner<3, 3>() = rot_hand_to_sensor;
		curHandToScanner_.topRightCorner<3, 1>() = tran_hand_to_sensor;

		w.head<6>() = R_tilde_star; w.tail<3>() = curHandToScanner_.topRightCorner<3, 1>();

		double rms_after = sqrt((A*w - Y).squaredNorm() / rows);

		std::cout << "After RMS: " << rms_after << "\n";

		if (rms_after < min_rms)
		{
			min_rms = rms_after;
			handToScanner_ = curHandToScanner_.cast<float>();
		}

		std::cout << "curHandToScanner:\n" << curHandToScanner_ << "\n";
	}

	std::cout << "\nmin RMS: " << min_rms << "\n final handToScanner:\n" << handToScanner_ << "\n";

	std::ofstream file("lineScannerHandEyeCalibration.bin", std::ios::out | std::ios::binary | std::ios::trunc);
	if (file.is_open())
	{
		for (int i = 0; i < 4; i++)
			for (int j = 0; j < 4; j++)
				file.write((char*)&handToScanner_.row(i)(j), sizeof(float));
	}
	else
	{
		std::cout << "save line scanner hand eye calibration fail\n" ;
	}

	file.close();
}

void VisionArmCombo::addArmModelToViewer(std::vector<PathPlanner::RefPoint>& ref_points)
{
	viewer_->removeAllShapes();

	for (int i = 0; i < ref_points.size()-1; i++)
	{
		pcl::ModelCoefficients cylinder_coeff;
		cylinder_coeff.values.resize(7);

		PointT p; p.x = ref_points[i].coordinates[0]; p.y = ref_points[i].coordinates[1]; p.z = ref_points[i].coordinates[2];
		PointT p1; p1.x = ref_points[i + 1].coordinates[0]; p1.y = ref_points[i + 1].coordinates[1]; p1.z = ref_points[i + 1].coordinates[2];

		cylinder_coeff.values[0] = p.x; cylinder_coeff.values[1] = p.y; cylinder_coeff.values[2] = p.z;
		cylinder_coeff.values[3] = p1.x - p.x; cylinder_coeff.values[4] = p1.y - p.y; cylinder_coeff.values[5] = p1.z - p.z;
		cylinder_coeff.values[6] = pp_.arm_radius_lookup[i];

		viewer_->addCylinder(cylinder_coeff, "cylinder" + std::to_string(i), 0);
	}

	display();;
}

void VisionArmCombo::addOBBArmModelToViewer(std::vector<PathPlanner::OBB> & arm_obbs)
{
	viewer_->removeAllShapes();
	viewer_->removeAllCoordinateSystems();
	viewer_->addCoordinateSystem(0.3, "world", 0);

	for (int j = 0; j < arm_obbs.size(); j++)
	{
		pcl::ModelCoefficients cube_coeff;
		cube_coeff.values.resize(10);
		for (int i = 0; i < 3; i++) cube_coeff.values[i] = arm_obbs[j].C(i);
		Eigen::Quaternionf quat(arm_obbs[j].A);

		cube_coeff.values[3] = quat.x();
		cube_coeff.values[4] = quat.y();
		cube_coeff.values[5] = quat.z();
		cube_coeff.values[6] = quat.w();

		cube_coeff.values[7] = arm_obbs[j].a(0)*2.f;
		cube_coeff.values[8] = arm_obbs[j].a(1)*2.f;
		cube_coeff.values[9] = arm_obbs[j].a(2)*2.f;

		viewer_->addCube(cube_coeff, "cube"+std::to_string(j), 0);

		Eigen::Affine3f transform;
		Eigen::Matrix4f temp_mat = Eigen::Matrix4f::Identity();
		temp_mat.block<3, 3>(0, 0) = arm_obbs[j].A;;
		temp_mat.block<3, 1>(0, 3) << arm_obbs[j].C(0), arm_obbs[j].C(1), arm_obbs[j].C(2);
		transform.matrix() = temp_mat;

		//if(j>0) std::cout << "obb collision of " <<j-1<<" and "<<j<<" = " << pp_.collisionOBB(arm_obbs[j-1], arm_obbs[j]) <<"\n";	
		//viewer->addCoordinateSystem(0.2, transform, "co"+std::to_string(j), 0);
	}

	display();;
}

void VisionArmCombo::showOccupancyGrid()
{
	if (!pp_.path_planner_ready_)	return;

	PointCloudT::Ptr cloud(new PointCloudT);

	for (int z = 0; z < pp_.grid_height_; z++)
	{
		for (int y = 0; y < pp_.grid_depth_; y++)
		{
			for (int x = 0; x < pp_.grid_width_; x++)
			{
				int cell_idx = x + y*pp_.grid_width_ + z*pp_.grid_depth_*pp_.grid_width_;

				PathPlanner::Cell cell = pp_.grid_[cell_idx];

				if (cell.isOccupiedCounter == pp_.prmce_round_counter_ )
				{
					PointT p;
					p.x = ((x + 0.5f)*pp_.cell_size_ + pp_.grid_offset_x_)*0.01f; 
					p.y = ((y + 0.5f)*pp_.cell_size_ + pp_.grid_offset_y_)*0.01f; 
					p.z = ((z + 0.5f)*pp_.cell_size_ + pp_.grid_offset_z_)*0.01f;
					p.r = p.g = p.b = 255;
					cloud->points.push_back(p);
				}

				if (cell.sweptVolumneCounter == pp_.prmce_swept_volume_counter_)
				{
					PointT p;
					p.x = ((x + 0.5f)*pp_.cell_size_ + pp_.grid_offset_x_)*0.01f;
					p.y = ((y + 0.5f)*pp_.cell_size_ + pp_.grid_offset_y_)*0.01f;
					p.z = ((z + 0.5f)*pp_.cell_size_ + pp_.grid_offset_z_)*0.01f;
					p.r = 255; p.g = p.b = 0;
					cloud->points.push_back(p);
				}
			}
		}
	}

	std::cout << "vox cloud size: " << cloud->points.size() << "\n";

	viewer_->addPointCloud(cloud, "grid", 0);

	display();
}

void VisionArmCombo::viewPlannedPath(float* start_pose, float* goal_pose)
{
	std::vector<PathPlanner::RefPoint> ref_points;
	std::vector<Eigen::Matrix3f> rot_mats;
	std::vector<PathPlanner::OBB> arm_obbs;

	std::cout << "start pose\n";

	// visualization
	pp_.computeReferencePointsOnArm(start_pose, ref_points, rot_mats);
	pp_.getArmOBBModel(ref_points, rot_mats, arm_obbs); addOBBArmModelToViewer(arm_obbs);

	for (auto index : pp_.shortest_path_index_vec_)
	{
		float config[6];

		memcpy(config, pp_.random_nodes_buffer_ + index * pp_.num_joints_, pp_.num_joints_ * sizeof(float));

		pp_.computeReferencePointsOnArm(config, ref_points, rot_mats);
		pp_.getArmOBBModel(ref_points, rot_mats, arm_obbs); addOBBArmModelToViewer(arm_obbs);
	}

	std::cout << "goal pose\n";

	pp_.computeReferencePointsOnArm(goal_pose, ref_points, rot_mats);
	pp_.getArmOBBModel(ref_points, rot_mats, arm_obbs); addOBBArmModelToViewer(arm_obbs);
}


/*
	https://github.com/ros-industrial/universal_robot/blob/indigo-devel/ur_kinematics/src/ur_kin.cpp
	Analytical solutions + picking the feasible one
*/
int VisionArmCombo::inverseKinematics(Eigen::Matrix4d & T, std::vector<int> & ik_sols_vec)
{
	ik_sols_vec.clear();
	const double q6_des = -PI;
	int num_sols = 0;
	/*double T02 = -*T; T++; double T00 = *T; T++; double T01 = *T; T++; double T03 = -*T; T++;
	double T12 = -*T; T++; double T10 = *T; T++; double T11 = *T; T++; double T13 = -*T; T++;
	double T22 = *T; T++; double T20 = -*T; T++; double T21 = -*T; T++; double T23 = *T;*/
	double T00 = T(0, 0); double T01 = T(0, 1); double T02 = T(0, 2); double T03 = T(0, 3);
	double T10 = T(1, 0); double T11 = T(1, 1); double T12 = T(1, 2); double T13 = T(1, 3);
	double T20 = T(2, 0); double T21 = T(2, 1); double T22 = T(2, 2); double T23 = T(2, 3);

	////////////////////////////// shoulder rotate joint (q1) //////////////////////////////
	double q1[2];
	{
		double A = d6*T12 - T13;
		double B = d6*T02 - T03;
		double R = A*A + B*B;
		if (fabs(A) < ZERO_THRESH) {
			double div;
			if (fabs(fabs(d4) - fabs(B)) < ZERO_THRESH)
				div = -SIGN(d4)*SIGN(B);
			else
				div = -d4 / B;
			double arcsin = asin(div);
			if (fabs(arcsin) < ZERO_THRESH)
				arcsin = 0.0;
			if (arcsin < 0.0)
				q1[0] = arcsin + 2.0*PI;
			else
				q1[0] = arcsin;
			q1[1] = PI - arcsin;
		}
		else if (fabs(B) < ZERO_THRESH) {
			double div;
			if (fabs(fabs(d4) - fabs(A)) < ZERO_THRESH)
				div = SIGN(d4)*SIGN(A);
			else
				div = d4 / A;
			double arccos = acos(div);
			q1[0] = arccos;
			q1[1] = 2.0*PI - arccos;
		}
		else if (d4*d4 > R) {
			return num_sols;
		}
		else {
			double arccos = acos(d4 / sqrt(R));
			double arctan = atan2(-B, A);
			double pos = arccos + arctan;
			double neg = -arccos + arctan;
			if (fabs(pos) < ZERO_THRESH)
				pos = 0.0;
			if (fabs(neg) < ZERO_THRESH)
				neg = 0.0;
			if (pos >= 0.0)
				q1[0] = pos;
			else
				q1[0] = 2.0*PI + pos;
			if (neg >= 0.0)
				q1[1] = neg;
			else
				q1[1] = 2.0*PI + neg;
		}
	}

	////////////////////////////// wrist 2 joint (q5) //////////////////////////////
	double q5[2][2];
	{
		for (int i = 0; i<2; i++) {
			double numer = (T03*sin(q1[i]) - T13*cos(q1[i]) - d4);
			double div;
			if (fabs(fabs(numer) - fabs(d6)) < ZERO_THRESH)
				div = SIGN(numer) * SIGN(d6);
			else
				div = numer / d6;
			double arccos = acos(div);
			q5[i][0] = arccos;
			q5[i][1] = 2.0*PI - arccos;
		}
	}
	////////////////////////////////////////////////////////////////////////////////
	{
		for (int i = 0; i<2; i++) {
			for (int j = 0; j<2; j++) {
				double c1 = cos(q1[i]), s1 = sin(q1[i]);
				double c5 = cos(q5[i][j]), s5 = sin(q5[i][j]);
				double q6;
				////////////////////////////// wrist 3 joint (q6) //////////////////////////////
				if (fabs(s5) < ZERO_THRESH)
					q6 = q6_des;
				else {
					q6 = atan2(SIGN(s5)*-(T01*s1 - T11*c1),
						SIGN(s5)*(T00*s1 - T10*c1));
					if (fabs(q6) < ZERO_THRESH)
						q6 = 0.0;
					if (q6 < 0.0)
						q6 += 2.0*PI;
				}
				////////////////////////////////////////////////////////////////////////////////

				double q2[2], q3[2], q4[2];
				///////////////////////////// RRR joints (q2,q3,q4) ////////////////////////////
				double c6 = cos(q6), s6 = sin(q6);
				double x04x = -s5*(T02*c1 + T12*s1) - c5*(s6*(T01*c1 + T11*s1) - c6*(T00*c1 + T10*s1));
				double x04y = c5*(T20*c6 - T21*s6) - T22*s5;
				double p13x = d5*(s6*(T00*c1 + T10*s1) + c6*(T01*c1 + T11*s1)) - d6*(T02*c1 + T12*s1) +
					T03*c1 + T13*s1;
				double p13y = T23 - d1 - d6*T22 + d5*(T21*c6 + T20*s6);

				double c3 = (p13x*p13x + p13y*p13y - a2*a2 - a3*a3) / (2.0*a2*a3);
				if (fabs(fabs(c3) - 1.0) < ZERO_THRESH)
					c3 = SIGN(c3);
				else if (fabs(c3) > 1.0) {
					// TODO NO SOLUTION
					continue;
				}
				double arccos = acos(c3);
				q3[0] = arccos;
				q3[1] = 2.0*PI - arccos;
				double denom = a2*a2 + a3*a3 + 2 * a2*a3*c3;
				double s3 = sin(arccos);
				double A = (a2 + a3*c3), B = a3*s3;
				q2[0] = atan2((A*p13y - B*p13x) / denom, (A*p13x + B*p13y) / denom);
				q2[1] = atan2((A*p13y + B*p13x) / denom, (A*p13x - B*p13y) / denom);
				double c23_0 = cos(q2[0] + q3[0]);
				double s23_0 = sin(q2[0] + q3[0]);
				double c23_1 = cos(q2[1] + q3[1]);
				double s23_1 = sin(q2[1] + q3[1]);
				q4[0] = atan2(c23_0*x04y - s23_0*x04x, x04x*c23_0 + x04y*s23_0);
				q4[1] = atan2(c23_1*x04y - s23_1*x04x, x04x*c23_1 + x04y*s23_1);
				////////////////////////////////////////////////////////////////////////////////
				for (int k = 0; k<2; k++) {
					if (fabs(q2[k]) < ZERO_THRESH)
						q2[k] = 0.0;
					else if (q2[k] < 0.0) q2[k] += 2.0*PI;
					if (fabs(q4[k]) < ZERO_THRESH)
						q4[k] = 0.0;
					else if (q4[k] < 0.0) q4[k] += 2.0*PI;
					ik_sols_[num_sols * 6 + 0] = q1[i];    ik_sols_[num_sols * 6 + 1] = q2[k];
					ik_sols_[num_sols * 6 + 2] = q3[k];    ik_sols_[num_sols * 6 + 3] = q4[k];
					ik_sols_[num_sols * 6 + 4] = q5[i][j]; ik_sols_[num_sols * 6 + 5] = q6;
					num_sols++;
				}
			}
		}
	}

	// the solution joint angle may not be in the range we want
	for (int i = 0; i < num_sols; i++)
	{
		bool valid_solution = true;

		// try to bring the joint angle back to the range we want
		for (int j = 0; j < 6; j++)
		{
			double min = joint_range_for_probe_[j * 2];
			double max = joint_range_for_probe_[j * 2 + 1];
			double q = ik_sols_[i * 6 + j];

			if (q > max) q -= 2 * PI;
			else if (q < min) q += 2 * PI;
			else continue;

			if (q <= max && q >= min) ik_sols_[i * 6 + j] = q;
			else
			{
				valid_solution = false;
				break;
			}
		}

		if (valid_solution)
		{
			ik_sols_vec.push_back(i);
		/*	std::cout << ik_sols_vec.back() << ": ";
			for (int k = 0; k < 6; k++)
				std::cout << ik_sols_[i * 6 + k] << " ";
			std::cout << "\n";*/
		}
	}

	return num_sols;
}


void VisionArmCombo::forward(const double* q, double* T)
{
	double s1 = sin(*q), c1 = cos(*q); q++;
	double q234 = *q, s2 = sin(*q), c2 = cos(*q); q++;
	double s3 = sin(*q), c3 = cos(*q); q234 += *q; q++;
	q234 += *q; q++;
	double s5 = sin(*q), c5 = cos(*q); q++;
	double s6 = sin(*q), c6 = cos(*q);
	double s234 = sin(q234), c234 = cos(q234);
	*T = ((c1*c234 - s1*s234)*s5) / 2.0 - c5*s1 + ((c1*c234 + s1*s234)*s5) / 2.0; T++;
	*T = (c6*(s1*s5 + ((c1*c234 - s1*s234)*c5) / 2.0 + ((c1*c234 + s1*s234)*c5) / 2.0) -
		(s6*((s1*c234 + c1*s234) - (s1*c234 - c1*s234))) / 2.0); T++;
	*T = (-(c6*((s1*c234 + c1*s234) - (s1*c234 - c1*s234))) / 2.0 -
		s6*(s1*s5 + ((c1*c234 - s1*s234)*c5) / 2.0 + ((c1*c234 + s1*s234)*c5) / 2.0)); T++;
	*T = ((d5*(s1*c234 - c1*s234)) / 2.0 - (d5*(s1*c234 + c1*s234)) / 2.0 -
		d4*s1 + (d6*(c1*c234 - s1*s234)*s5) / 2.0 + (d6*(c1*c234 + s1*s234)*s5) / 2.0 -
		a2*c1*c2 - d6*c5*s1 - a3*c1*c2*c3 + a3*c1*s2*s3); T++;
	*T = c1*c5 + ((s1*c234 + c1*s234)*s5) / 2.0 + ((s1*c234 - c1*s234)*s5) / 2.0; T++;
	*T = (c6*(((s1*c234 + c1*s234)*c5) / 2.0 - c1*s5 + ((s1*c234 - c1*s234)*c5) / 2.0) +
		s6*((c1*c234 - s1*s234) / 2.0 - (c1*c234 + s1*s234) / 2.0)); T++;
	*T = (c6*((c1*c234 - s1*s234) / 2.0 - (c1*c234 + s1*s234) / 2.0) -
		s6*(((s1*c234 + c1*s234)*c5) / 2.0 - c1*s5 + ((s1*c234 - c1*s234)*c5) / 2.0)); T++;
	*T = ((d5*(c1*c234 - s1*s234)) / 2.0 - (d5*(c1*c234 + s1*s234)) / 2.0 + d4*c1 +
		(d6*(s1*c234 + c1*s234)*s5) / 2.0 + (d6*(s1*c234 - c1*s234)*s5) / 2.0 + d6*c1*c5 -
		a2*c2*s1 - a3*c2*c3*s1 + a3*s1*s2*s3); T++;
	*T = ((c234*c5 - s234*s5) / 2.0 - (c234*c5 + s234*s5) / 2.0); T++;
	*T = ((s234*c6 - c234*s6) / 2.0 - (s234*c6 + c234*s6) / 2.0 - s234*c5*c6); T++;
	*T = (s234*c5*s6 - (c234*c6 + s234*s6) / 2.0 - (c234*c6 - s234*s6) / 2.0); T++;
	*T = (d1 + (d6*(c234*c5 - s234*s5)) / 2.0 + a3*(s2*c3 + c2*s3) + a2*s2 -
		(d6*(c234*c5 + s234*s5)) / 2.0 - d5*c234); T++;
	*T = 0.0; T++; *T = 0.0; T++; *T = 0.0; T++; *T = 1.0;
}

void VisionArmCombo::float2double(float* array6_f, double* array6_d)
{
	for (int i = 0; i < 6; i++) array6_d[i] = array6_f[i];
}

void VisionArmCombo::double2float(double* array6_d, float* array6_f)
{
	for (int i = 0; i < 6; i++) array6_f[i] = array6_d[i];
}

bool VisionArmCombo::moveToConfigGetKinectPointCloud(ArmConfig & config, bool get_cloud = true, bool try_direct_path = true)
{
	if (!pp_.path_planner_ready_ || robot_arm_client_ == NULL)
	{
		std::cout << "path planner OR robot arm client not ready\n";
		return false;
	}

	if (pp_.collisionCheckForSingleConfig(config.joint_pos_f))
	{
		std::cout << "collision found\n";
		return false;
	}

	double start_pose_d[6];
	robot_arm_client_->getCurJointPose(start_pose_d);

	float start_pose[6];
	for (int i = 0; i < 6; i++) start_pose[i] = (float)start_pose_d[i];

	double start_to_goal_L2 = L2Norm(start_pose_d, config.joint_pos_d);
	//std::cout << "L2 NORM: " << start_to_goal_L2 << "\n";

	if (start_to_goal_L2 > 1e-3)	//away from goal
	{
		if (!pp_.planPath(start_pose, config.joint_pos_f, true, try_direct_path))
		{
			std::cout << "collsion-free path not found\n";
			viewPlannedPath(start_pose, config.joint_pos_f);
			return false;
		}

		viewPlannedPath(start_pose, config.joint_pos_f);

		for (int i = 0; i < pp_.shortest_path_index_vec_.size(); i++)
		{
			float config_f[6];

			memcpy(config_f, pp_.random_nodes_buffer_ + pp_.shortest_path_index_vec_[i] * pp_.num_joints_, pp_.num_joints_*sizeof(float));

			double config_d[6];
			float2double(config_f, config_d);
			robot_arm_client_->moveHandJ(config_d, move_joint_speed_, move_joint_acceleration_, true);
			//std::getchar();
		}

		robot_arm_client_->moveHandJ(config.joint_pos_d, move_joint_speed_, move_joint_acceleration_, true);
	}

	if (get_cloud)
	{
		// get point cloud from kinect
		pcl::PointCloud<pcl::PointXYZRGB>::Ptr point_cloud(new pcl::PointCloud<pcl::PointXYZRGB>);

		kinect_thread_->getCurPointCloud(point_cloud);

	/*	viewer_->removeAllPointClouds();
		viewer_->addPointCloud(point_cloud);
		display();*/

		// depth value correction
		for (int i = 0; i < point_cloud->points.size(); i++)
			point_cloud->points[i].z -= 0.024f;

		// get robot hand pose
		double array6[6];

		Eigen::Matrix4f hand2base;

		robot_arm_client_->getCartesianInfo(array6);

		array6ToEigenMat4(array6, hand2base);

		Eigen::Matrix4f cam2base;

		cam2base = hand2base*cam2hand_kinect_;

		PointCloudT::Ptr cloud_in_base(new PointCloudT);

		pcl::transformPointCloud(*point_cloud, *cloud_in_base, cam2base);

		*kinect_cloud_ += *cloud_in_base;

		// voxel grid donwsample
		vox_.setInputCloud(kinect_cloud_);
		vox_.setLeafSize(voxel_grid_size_, voxel_grid_size_, voxel_grid_size_);

		point_cloud->clear();
		vox_.filter(*point_cloud);

		kinect_cloud_->clear();

		*kinect_cloud_ += *point_cloud;

		pp_.addPointCloudToOccupancyGrid(kinect_cloud_);

		viewer_->removeAllPointClouds();

		viewer_->addPointCloud(kinect_cloud_, "cloud", 0);

		viewer_->setPointCloudRenderingProperties(pcl::visualization::PCL_VISUALIZER_POINT_SIZE, 2, "cloud");

		display();
	}

	return true;
}

double VisionArmCombo::L2Norm(double* array6_1, double* array6_2)
{
	double distance = 0.;
	for (int i = 0; i < 6; i++)
	{
		double  r = array6_1[i] - array6_2[i];
		distance += r*r;
	}
	return sqrt(distance);
}

void VisionArmCombo::processGrowthChamberEnviroment(PointCloudT::Ptr cloud_in_arm_base, float shelf_z_value, int num_plants)
{
	// keep points above shelf 
	PointCloudT::Ptr cloud(new PointCloudT);
	pass_.setInputCloud(cloud_in_arm_base);
	pass_.setFilterFieldName("z");
	pass_.setFilterLimits(shelf_z_value, 1.28f);
	pass_.setFilterLimitsNegative(false);
	pass_.filter(*cloud);

	*cloud_in_arm_base = *cloud;
	pass_.setInputCloud(cloud_in_arm_base);
	pass_.setFilterFieldName("y");
	pass_.setFilterLimits(-1.1f, -0.3f);
	pass_.setFilterLimitsNegative(false);
	pass_.filter(*cloud);

	// remove small clusters
	*cloud_in_arm_base = *cloud;
	smallClusterRemoval(cloud_in_arm_base, 0.02, 10, cloud);

	// opencv kmeans
	std::vector<cv::Point3f> cv_points(cloud->points.size());
	cv::Mat labels;

	for (int i = 0; i < cloud->points.size(); i++)
	{
		cv::Point3f point;
		point.x = cloud->points[i].x;
		point.y = cloud->points[i].y;
		point.z = cloud->points[i].z;
		cv_points[i] = point;
	}

	cv::kmeans(cv_points, num_plants, labels, cv::TermCriteria(cv::TermCriteria::EPS + cv::TermCriteria::COUNT, 40, 0.001), 10, cv::KMEANS_PP_CENTERS, object_centers_);

	std::vector<PointCloudT::Ptr> clusters(num_plants);
	
	// generate some random colors
	cv::RNG rng;

	std::vector<float> colors(num_plants);

	for (int i = 0; i < num_plants; i++)
	{
		PointCloudT::Ptr cloud(new PointCloudT);
		clusters[i] = cloud;
		uint32_t rgb = ((uint32_t)rng.uniform(0, 255) << 16 | (uint32_t)rng.uniform(0, 255) << 8 | (uint32_t)rng.uniform(0, 255));
		colors[i] = *reinterpret_cast<float*>(&rgb);
	}
	
	for (int i = 0; i < cv_points.size(); i++)
	{
		int label = labels.at<int>(i, 0);
		PointT point;
		point.x = cv_points[i].x;
		point.y = cv_points[i].y;
		point.z = cv_points[i].z;
		point.rgb = colors[label];
		
		clusters[label]->push_back(point);
	}

	viewer_->removeAllPointClouds(0);


	std::vector<float> cluster_radius_vec(num_plants);
	std::vector<Eigen::Vector4f> cluster_center_vec(num_plants);

	for (int i = 0; i < num_plants; i++)
	{
		Eigen::Vector4f min, max;

		pcl::getMinMax3D(*clusters[i], min, max);

		cluster_radius_vec[i] = (max(0) - min(0))*0.5f;

		cluster_center_vec[i] << 0.5f*(max + min);

		std::string name = "cluster" + std::to_string(i);

		viewer_->addPointCloud(clusters[i], name, 0);

		viewer_->setPointCloudRenderingProperties(pcl::visualization::PCL_VISUALIZER_POINT_SIZE, 2, name);
	}
	
	for (int i = 0; i < num_plants; i++)
	{
		pcl::ModelCoefficients coeffs;
		coeffs.values.resize(4);
		coeffs.values[0] = object_centers_.at<cv::Vec3f>(i, 0)[0];
		coeffs.values[1] = object_centers_.at<cv::Vec3f>(i, 0)[1];
		coeffs.values[2] = object_centers_.at<cv::Vec3f>(i, 0)[2];
		coeffs.values[3] = cluster_radius_vec[i];

		viewer_->addSphere(coeffs,"sphere" + std::to_string(i), 0);
	}

	display();


	// scan each cluster
	for (int i = 0; i < num_plants; i++)
	{
		PointT point;
		point.x = object_centers_.at<cv::Vec3f>(i, 0)[0]; //cluster_center_vec[i](0);
		point.y = object_centers_.at<cv::Vec3f>(i, 0)[1]; //cluster_center_vec[i](1);
		point.z = object_centers_.at<cv::Vec3f>(i, 0)[2];

		pcl::Normal normal(0.f, 0.f, -1.f);

		std::vector<ArmConfig> solution_config_vec;
		Eigen::Matrix4d scan_start_hand_pose;
		Eigen::Vector3d hand_translation;

		setScanRadius(cluster_radius_vec[i]);

		// compute laser scan pose
		if (computeCollisionFreeProbeOrScanPose(point, normal, false, solution_config_vec, scan_start_hand_pose, hand_translation))
		{
			//std::cout << "hand translation: " << hand_translation.transpose() << "\n";

			// move to the start scan pose
			if (moveToConfigGetKinectPointCloud(solution_config_vec[0], false, true))
			{
				double vec[3];
				vec[0] = hand_translation(0);
				vec[1] = hand_translation(1);
				vec[2] = hand_translation(2);

				PointCloudT::Ptr scan_cloud(new PointCloudT);

				//wait 1 sec
				std::cout << "scan plant " << i << "\n";
				Sleep(800);

				// moveToConfig moves the arm to a pose slightly different from scan_start_hand_pose
				Eigen::Matrix4f curPose; getCurHandPose(curPose);

				//std::cout << curPose << "\n" << scan_start_hand_pose << "\n";

				scan_start_hand_pose = curPose.cast<double>();


				// Use laser scanner
				scanTranslateOnly(vec, scan_cloud, scan_acceleration_, scan_speed_);

				PointCloudT::Ptr scan_cloud_down(new PointCloudT);

				vox_.setLeafSize(0.001, 0.001, 0.001);

				vox_.setInputCloud(scan_cloud);

				vox_.filter(*scan_cloud_down);

				sor_.setInputCloud(scan_cloud_down);

				PointCloudT::Ptr scan_cloud_down_filter(new PointCloudT);

				sor_.filter(*scan_cloud_down_filter);

				PointCloudT::Ptr scan_cloud_in_base(new PointCloudT);

				pcl::transformPointCloud(*scan_cloud_down_filter, *scan_cloud_in_base, scan_start_hand_pose.cast<float>()*handToScanner_);
				
				*laser_cloud_ += *scan_cloud_in_base;

				vox_.setInputCloud(laser_cloud_);

				scan_cloud_in_base->clear();

				vox_.filter(*scan_cloud_in_base);
				
				*laser_cloud_ = *scan_cloud_in_base;

				//pcl::io::savePCDFileBinary("laser_scan.pcd", *scan_cloud_in_base);

				pp_.resetOccupancyGrid();

				pp_.addPointCloudToOccupancyGrid(laser_cloud_);

				//showOccupancyGrid();

				std::vector<pcl::PointXYZRGBNormal> probe_pn_vec;

				std::cout << "Extract leaf probing points...\n";

				extractLeafProbingPoints(laser_cloud_, probe_pn_vec);

				int leaf_id = 0;

				for (auto pn : probe_pn_vec)
				{
					std::cout << "probe leaf id " << leaf_id++ << "\n";
					PointT p;
					p.x = pn.x; p.y = pn.y; p.z = pn.z;
					pcl::Normal n;
					n.normal_x = pn.normal_x; n.normal_y = pn.normal_y; n.normal_z = pn.normal_z;
					probeLeaf(p, n);
				}

				counter_++;

				probeScannedSceneTest(scan_cloud_in_base);

				//viewer_->addPointCloud(scan_cloud_in_base, "scancloud" + std::to_string(i), 0);

				//display();
			}
		}
	}

	pcl::MinCutSegmentation<PointT> seg;

	seg.setInputCloud(cloud);

	for (int i = 0; i < num_plants; i++)
	{
		PointCloudT::Ptr foreground_points(new PointCloudT);
		PointT point;
		point.x = object_centers_.at<cv::Vec3f>(i, 0)[0];
		point.y = object_centers_.at<cv::Vec3f>(i, 0)[1];
		point.z = object_centers_.at<cv::Vec3f>(i, 0)[2];

		foreground_points->points.push_back(point);
		seg.setForegroundPoints(foreground_points);

		PointCloudT::Ptr background_points(new PointCloudT);
		for (int j = 0; j < num_plants; j++)
		{
			if (j == i) continue;

			PointT bpoint;
			bpoint.x = object_centers_.at<cv::Vec3f>(j, 0)[0];
			bpoint.y = object_centers_.at<cv::Vec3f>(j, 0)[1];
			bpoint.z = object_centers_.at<cv::Vec3f>(j, 0)[2];
			background_points->points.push_back(bpoint);
		}

		seg.setBackgroundPoints(background_points);

		seg.setSigma(0.2);
		seg.setRadius(0.1);
		seg.setNumberOfNeighbours(10);
		seg.setSourceWeight(0.8);

		std::vector <pcl::PointIndices> clusters;
		seg.extract(clusters);

		PointCloudT::Ptr colored_cloud = seg.getColoredCloud();
		viewer_->removeAllPointClouds();
		viewer_->addPointCloud(colored_cloud);
		display();
	}

	kinect_cloud_->clear();
	*kinect_cloud_ += *cloud;

	viewer_->removeAllPointClouds();
	viewer_->addPointCloud(cloud);
	display();
}

void VisionArmCombo::addSupervoxelConnectionsToViewer(PointT &supervoxel_center,
											PointCloudT &adjacent_supervoxel_centers, std::string name,
											boost::shared_ptr<pcl::visualization::PCLVisualizer> & viewer)
{
	for (int i = 0; i < adjacent_supervoxel_centers.points.size(); i++)
	{
		pcl::ModelCoefficients line;
		line.values.resize(6);
		line.values[0] = supervoxel_center.x; line.values[1] = supervoxel_center.y; line.values[2] = supervoxel_center.z;
		line.values[3] = adjacent_supervoxel_centers.points[i].x - supervoxel_center.x;
		line.values[4] = adjacent_supervoxel_centers.points[i].y - supervoxel_center.y;
		line.values[5] = adjacent_supervoxel_centers.points[i].z - supervoxel_center.z;

		std::string line_name = name + std::to_string(i);

		viewer->addLine(line, line_name, 0);
	}

	//vtkSmartPointer<vtkPoints> points = vtkSmartPointer<vtkPoints>::New();
	//vtkSmartPointer<vtkCellArray> cells = vtkSmartPointer<vtkCellArray>::New();
	//vtkSmartPointer<vtkPolyLine> polyLine = vtkSmartPointer<vtkPolyLine>::New();

	////Iterate through all adjacent points, and add a center point to adjacent point pair
	//PointCloudT::iterator adjacent_itr = adjacent_supervoxel_centers.begin();
	//for (; adjacent_itr != adjacent_supervoxel_centers.end(); ++adjacent_itr)
	//{
	//	points->InsertNextPoint(supervoxel_center.data);
	//	points->InsertNextPoint(adjacent_itr->data);
	//}
	//// Create a polydata to store everything in
	//vtkSmartPointer<vtkPolyData> polyData = vtkSmartPointer<vtkPolyData>::New();
	//// Add the points to the dataset
	//polyData->SetPoints(points);
	//polyLine->GetPointIds()->SetNumberOfIds(points->GetNumberOfPoints());
	//for (unsigned int i = 0; i < points->GetNumberOfPoints(); i++)
	//	polyLine->GetPointIds()->SetId(i, i);
	//cells->InsertNextCell(polyLine);
	//// Add the lines to the dataset
	//polyData->SetLines(cells);
	//viewer->addModelFromPolyData(polyData, supervoxel_name);
}

void VisionArmCombo::extractProbeSurfacePatchFromPointCloud(PointCloudT::Ptr cloud, std::vector<pcl::Supervoxel<PointT>::Ptr> & potential_probe_supervoxels)
{
	// supervoxel
	float seed_resolution = 0.04f;
	float color_importance = 0.f;
	float spatial_importance = 1.f;
	float normal_importance = 0.0f;

	pcl::SupervoxelClustering<PointT> super(voxel_grid_size_, seed_resolution);
	super.setInputCloud(cloud);
	super.setColorImportance(color_importance);
	super.setSpatialImportance(spatial_importance);
	super.setNormalImportance(normal_importance);
	std::map <uint32_t, pcl::Supervoxel<PointT>::Ptr > supervoxel_clusters;
	super.extract(supervoxel_clusters);

	viewer_->removeAllPointClouds();
	/*PointCloudT::Ptr voxel_centroid_cloud = super.getVoxelCentroidCloud();
	viewer_->addPointCloud(voxel_centroid_cloud, "voxel centroids");
	viewer_->setPointCloudRenderingProperties(pcl::visualization::PCL_VISUALIZER_POINT_SIZE, 2.0, "voxel centroids");
	viewer_->setPointCloudRenderingProperties(pcl::visualization::PCL_VISUALIZER_OPACITY, 0.95, "voxel centroids");
	*/
	PointLCloudT::Ptr labeled_voxel_cloud = super.getLabeledVoxelCloud();
	viewer_->addPointCloud(labeled_voxel_cloud, "labeled voxels");
	//viewer_->setPointCloudRenderingProperties(pcl::visualization::PCL_VISUALIZER_OPACITY, 1.0, "labeled voxels");

	pcl::SupervoxelClustering<PointT>::VoxelAdjacencyList supervoxelAdjacencyList;

	// label and edge length
	super.getSupervoxelAdjacencyList(supervoxelAdjacencyList);

	typedef pcl::SupervoxelClustering<PointT>::VoxelAdjacencyList::vertex_iterator sv_vertex_iterator;
	typedef pcl::SupervoxelClustering<PointT>::VoxelAdjacencyList::edge_iterator sv_edge_iterator;
	typedef pcl::SupervoxelClustering<PointT>::VoxelID VoxelID;	//descriptor
	typedef pcl::SupervoxelClustering<PointT>::EdgeID EdgeID;

	//sv_vertex_iterator vit, vend;
	//std::tie(vit, vend) = boost::vertices(supervoxelAdjacencyList);
	//int id = 1;
	//for (sv_vertex_iterator it = vit; it != vend; it++)
	//{
	
	//	std::cout <<"id "<<id++<< " "<<supervoxelAdjacencyList[*it] << "\n";
	//}

	//sv_edge_iterator eit, eend;
	//std::tie(eit, eend) = boost::edges(supervoxelAdjacencyList);

	//for (sv_edge_iterator it = eit; it != eend; it++)
	//{
	////	std::cout <<"edge weight "<< supervoxelAdjacencyList[(*it)] << "\n";

	//	//std::cout<<supervoxelAdjacencyList[(*it).m_source]<<"\n";
	//	
	//}

	//PointLCloudT::Ptr supervoxel_cloud(new PointLCloudT);
	//PointNCloudT::Ptr sv_normal_cloud = super.makeSupervoxelNormalCloud(supervoxel_clusters);
	////We have this disabled so graph is easy to see, uncomment to see supervoxel normals
	////viewer_->addPointCloudNormals<pcl::PointNormal> (sv_normal_cloud,1,0.05f, "supervoxel_normals");
	
	std::multimap<uint32_t, uint32_t> supervoxel_adjacency;
	super.getSupervoxelAdjacency(supervoxel_adjacency);

	potential_probe_supervoxels.clear();

	//To make a graph of the supervoxel adjacency, we need to iterate through the supervoxel adjacency multimap
	std::multimap<uint32_t, uint32_t>::iterator label_itr = supervoxel_adjacency.begin();
	for (; label_itr != supervoxel_adjacency.end(); )
	{
		//First get the label
		uint32_t supervoxel_label = label_itr->first;
		//Now get the supervoxel corresponding to the label
		pcl::Supervoxel<PointT>::Ptr supervoxel = supervoxel_clusters.at(supervoxel_label);

		if (supervoxel->voxels_->size() > 30)
		{
			// PCA 
			pcl::PCA<PointT> pca(*supervoxel->voxels_);
			Eigen::Vector3f eigen_values = pca.getEigenValues();
			//std::cout << "size "<< supervoxel->voxels_->points.size()<<" min eigen value "<< eigen_values(2) << "\n";

			if (eigen_values(2) < 0.002f && supervoxel->voxels_->points.size() > 60 )
			{
				potential_probe_supervoxels.push_back(supervoxel);
				pcl::ModelCoefficients line; line.values.resize(6);
				line.values[0] = supervoxel->centroid_.x;
				line.values[1] = supervoxel->centroid_.y;
				line.values[2] = supervoxel->centroid_.z;
				float sign = supervoxel->normal_.normal_z >= 0 ? 1.0f : -1.0f;
				line.values[3] = supervoxel->normal_.normal_x*0.02f*sign;
				line.values[4] = supervoxel->normal_.normal_y*0.02f*sign;
				line.values[5] = supervoxel->normal_.normal_z*0.02f*sign;
				viewer_->addLine(line, "l" + std::to_string(supervoxel_label), 0);
				//viewer_->addPointCloud(supervoxel->voxels_, "c" + std::to_string(supervoxel_label), 0);
			}

			//Now we need to iterate through the adjacent supervoxels and make a point cloud of them
			/*PointCloudT adjacent_supervoxel_centers;
			std::multimap<uint32_t, uint32_t>::iterator adjacent_itr = supervoxel_adjacency.equal_range(supervoxel_label).first;
			for (; adjacent_itr != supervoxel_adjacency.equal_range(supervoxel_label).second; ++adjacent_itr)
			{
				pcl::Supervoxel<PointT>::Ptr neighbor_supervoxel = supervoxel_clusters.at(adjacent_itr->second);
				pcl::PointXYZRGB p;
				p.x = neighbor_supervoxel->centroid_.x;
				p.y = neighbor_supervoxel->centroid_.y;
				p.z = neighbor_supervoxel->centroid_.z;
				p.r = neighbor_supervoxel->centroid_.r;
				p.g = neighbor_supervoxel->centroid_.g;
				p.b = neighbor_supervoxel->centroid_.b;
				adjacent_supervoxel_centers.push_back(p);
			}
				*/

			////Now we make a name for this polygon
			////This function is shown below, but is beyond the scope of this tutorial - basically it just generates a "star" polygon mesh from the points given
			//pcl::PointXYZRGB centroid;
			//centroid.x = supervoxel->centroid_.x;
			//centroid.y = supervoxel->centroid_.y;
			//centroid.z = supervoxel->centroid_.z;
			//centroid.r = supervoxel->centroid_.r;
			//centroid.g = supervoxel->centroid_.g;
			//centroid.b = supervoxel->centroid_.b;
			//addSupervoxelConnectionsToViewer(centroid, adjacent_supervoxel_centers, std::to_string(supervoxel_label), viewer_);
		}
		//Move iterator forward to next label
		label_itr = supervoxel_adjacency.upper_bound(supervoxel_label);
	}

	display();
}

bool VisionArmCombo::computeCollisionFreeProbeOrScanPose(PointT & point, pcl::Normal & normal, bool probe, std::vector<ArmConfig> & solution_config_vec,
															Eigen::Matrix4d & scan_start_or_probe_hand_pose, Eigen::Vector3d & hand_translation)
{
	Eigen::Vector3d z_dir;

	// make z point down
	if (normal.normal_z > 0) z_dir << -normal.normal_x, -normal.normal_y, -normal.normal_z;
	else z_dir << normal.normal_x, normal.normal_y, normal.normal_z;

	pcl::ModelCoefficients line; line.values.resize(6);
	line.values[0] = point.x;
	line.values[1] = point.y;
	line.values[2] = point.z;
	line.values[3] = -z_dir(0)*0.1;
	line.values[4] = -z_dir(1)*0.1;
	line.values[5] = -z_dir(2)*0.1;

	std::string name = "line";
	viewer_->removeAllShapes();
	viewer_->addLine(line, name, 0);
	display();

	// favor y pointing forward (Enviratron) (y<0)
	// sampling y from (1,0,0) to (-1,0,0)
	Eigen::Vector3d init_y_dir;
	Eigen::Matrix4d pose_to_hand;
	if (probe)
	{
		init_y_dir << 1.0, 0.0, 0.0;
		pose_to_hand = probe_to_hand_;
	}
	else
	{
		init_y_dir << 0.0, 1.0, 0.0;	// scanner
		pose_to_hand = scan_start_to_hand_;
	}

	// project to plane
	init_y_dir = init_y_dir - z_dir*(init_y_dir.dot(z_dir));

	//std::cout << "init_y_dir " << init_y_dir.transpose() << "\n";

	double step_size = 10. / 180.*M_PI;

	solution_config_vec.clear();

	std::vector<double> y_value_vec;

	double min_y = 1.0f;

	Eigen::Matrix4d optimal_hand_pose;

	ArmConfig optimal_config;

	double optimal_angle;

	bool solution_found = false;
	
	for (double angle = 0.; angle <= M_PI; angle += step_size)
	{
		Eigen::Vector3d y_dir = Eigen::AngleAxisd(angle, z_dir) * init_y_dir;

		//std::cout << "y_dir " << y_dir.transpose() << "\n";

		Eigen::Vector3d x_dir = y_dir.cross(z_dir);

		// robot hand pose
		Eigen::Matrix4d pose = Eigen::Matrix4d::Identity();

		pose.col(0).head<3>() = x_dir;
		pose.col(1).head<3>() = y_dir;
		pose.col(2).head<3>() = z_dir;
		pose.col(3).head<3>() << point.x, point.y, point.z;

		pose = pose * pose_to_hand;

		// visualization
		/*Eigen::Affine3f affine_pose;
		affine_pose.matrix() = pose.cast<float>();
		viewer_->removeAllCoordinateSystems();
		viewer_->addCoordinateSystem(0.2, affine_pose, "co", 0);
		display();*/

		std::vector<int> ik_sols_vec;

		inverseKinematics(pose, ik_sols_vec);

		//if (ik_sols_vec.size() == 0) std::cout << "no ik solution\n";
		

		for (auto idx : ik_sols_vec)
		{
			float sol_f[6];

			double2float(ik_sols_ + idx * num_joints_, sol_f);

			if (!pp_.collisionCheckForSingleConfig(sol_f) && pose(1, 1) < min_y)
			{
				optimal_hand_pose = pose;

				optimal_angle = angle;

				min_y = pose(1, 1);

				optimal_config.setJointPos(ik_sols_[idx*num_joints_], ik_sols_[idx*num_joints_ + 1],
									ik_sols_[idx*num_joints_ + 2], ik_sols_[idx*num_joints_ + 3],
									ik_sols_[idx*num_joints_ + 4], ik_sols_[idx*num_joints_ + 5]);

				solution_found = true;
			}
			//else std::cout << "collision\n";
		}
	}

	if (!solution_found)
	{
		std::cout << "no collision free pose found\n";
		return solution_found;
	}

	std::vector<PathPlanner::RefPoint> ref_points;
	std::vector<Eigen::Matrix3f> rot_mats;
	std::vector<PathPlanner::OBB> arm_obbs;

	// visualization
	pp_.computeReferencePointsOnArm(optimal_config.joint_pos_f, ref_points, rot_mats);
	pp_.getArmOBBModel(ref_points, rot_mats, arm_obbs); addOBBArmModelToViewer(arm_obbs);

	solution_config_vec.push_back(optimal_config);

	scan_start_or_probe_hand_pose = optimal_hand_pose;

	if (probe) return solution_found;
	
	pose_to_hand = scan_end_to_hand_;

	Eigen::Vector3d y_dir = Eigen::AngleAxisd(optimal_angle, z_dir) * init_y_dir;

	//std::cout << "scan end y_dir " << y_dir.transpose() << "\n";

	Eigen::Vector3d x_dir = y_dir.cross(z_dir);

	Eigen::Matrix4d pose = Eigen::Matrix4d::Identity();

	pose.col(0).head<3>() = x_dir;
	pose.col(1).head<3>() = y_dir;
	pose.col(2).head<3>() = z_dir;
	pose.col(3).head<3>() << point.x, point.y, point.z;

	pose = pose * pose_to_hand;

	Eigen::Affine3f affine_pose;
	affine_pose.matrix() = pose.cast<float>();
	viewer_->removeAllCoordinateSystems();
	viewer_->addCoordinateSystem(0.2, affine_pose, "co", 0);
	display();

	std::vector<int> ik_sols_vec;

	inverseKinematics(pose, ik_sols_vec);

	solution_found = false;

	float config[6];

	for (auto idx : ik_sols_vec)
	{
		float sol_f[6];

		double2float(ik_sols_ + idx * num_joints_, sol_f);

		if (!pp_.collisionCheckForSingleConfig(sol_f))
		{
			double2float(ik_sols_ + idx * num_joints_, config);

			solution_found = true;
		}
	}

	if (!solution_found)
	{
		solution_config_vec.clear();
		std::cout << "scan end pose not found\n";
		return solution_found;
	}


	// visualization
	std::cout << "scan end pose\n";
	pp_.computeReferencePointsOnArm(config, ref_points, rot_mats);
	pp_.getArmOBBModel(ref_points, rot_mats, arm_obbs); addOBBArmModelToViewer(arm_obbs);

	hand_translation = pose.col(3).head<3>() - optimal_hand_pose.col(3).head<3>();
	scan_start_or_probe_hand_pose = optimal_hand_pose;
	return solution_found;
}

void VisionArmCombo::getCurHandPose(Eigen::Matrix4f & pose)
{
	double array6[6];

	robot_arm_client_->getCartesianInfo(array6);

	array6ToEigenMat4(array6, pose);
}

void VisionArmCombo::getCurHandPoseD(Eigen::Matrix4d & pose)
{
	double array6[6];

	robot_arm_client_->getCartesianInfo(array6);

	array6ToEigenMat4d(array6, pose);
}

void VisionArmCombo::probeScannedSceneTest(PointCloudT::Ptr cloud)
{
	//viewer_->removeAllPointClouds();
	viewer_->addPointCloud(cloud, "probeScannedCloud"+std::to_string(counter_), 0);

	while (true)
	{
		display();

		std::cout << "move hand?\n";

		std::string command;
		std::getline(std::cin, command);
		
		if (command == "y")
		{
			double pos[6];

			Eigen::Matrix4f base2hand;
			robot_arm_client_->getCartesianInfo(pos);
			array6ToEigenMat4(pos, base2hand);

			Eigen::Vector3f trans = pre_point_ - base2hand.topLeftCorner<3, 3>()*tool_center_point_;

			pos[0] = trans(0); pos[1] = trans(1); pos[2] = trans(2);

			robot_arm_client_->moveHandL(pos, move_arm_acceleration_, move_arm_speed_);
		}
		else if (command == "q")
		{
			break;
		}
	}
}

void VisionArmCombo::smallClusterRemoval(PointCloudT::Ptr cloud_in, double clusterTolerance, int minClusterSize, PointCloudT::Ptr cloud_out)
{
	// Euclidean cluster, remove small clusters
	pcl::search::KdTree<PointT>::Ptr tree(new pcl::search::KdTree<PointT>);
	std::vector<pcl::PointIndices> cluster_indices;
	pcl::EuclideanClusterExtraction<PointT> ec;
	ec.setClusterTolerance(clusterTolerance); //distance m
	ec.setMinClusterSize(1);
	ec.setMaxClusterSize(cloud_in->points.size());
	ec.setSearchMethod(tree);
	ec.setInputCloud(cloud_in);
	ec.extract(cluster_indices);

	cloud_out->points.clear();

	for (int j = 0; j<cluster_indices.size(); j++)
	{
		if (cluster_indices[j].indices.size() > minClusterSize)
			for (int i = 0; i<cluster_indices[j].indices.size(); i++)
				cloud_out->push_back(cloud_in->points[cluster_indices[j].indices[i]]);
	}
}

void VisionArmCombo::setScanRadius(float radius)
{
	scan_start_to_hand_(1, 3) = -radius;
	scan_end_to_hand_(1, 3) = radius;
}

void VisionArmCombo::extractLeafProbingPoints(PointCloudT::Ptr cloud_in, std::vector<pcl::PointXYZRGBNormal> & probe_pn_vec)
{
	//region growing
	pcl::search::Search<PointT>::Ptr tree = boost::shared_ptr<pcl::search::Search<PointT> >(new pcl::search::KdTree<PointT>);
	pcl::PointCloud <pcl::Normal>::Ptr normals(new pcl::PointCloud <pcl::Normal>);
	pcl::NormalEstimation<PointT, pcl::Normal> normal_estimator;
	normal_estimator.setSearchMethod(tree);
	normal_estimator.setInputCloud(cloud_in);
	normal_estimator.setKSearch(50);
	normal_estimator.compute(*normals);

	pcl::RegionGrowing<PointT, pcl::Normal> reg;
	reg.setMinClusterSize(1000);
	reg.setMaxClusterSize(1000000);
	reg.setSearchMethod(tree);
	reg.setNumberOfNeighbours(30);
	reg.setInputCloud(cloud_in);
	//reg.setIndices (indices);
	reg.setInputNormals(normals);
	reg.setResidualThreshold(0.005f);
	reg.setSmoothnessThreshold(3.0 / 180.0 * M_PI);
	reg.setCurvatureThreshold(0.8);

	std::vector<pcl::PointIndices> clusters;
	reg.extract(clusters);

	PointCloudT::Ptr colored_cloud = reg.getColoredCloud();
	viewer_->removeAllPointClouds();
	viewer_->addPointCloud(colored_cloud, "region", 0);
	viewer_->setPointCloudRenderingProperties(pcl::visualization::PCL_VISUALIZER_POINT_SIZE, 2, "region");
	//display();

	probe_pn_vec.clear();

	for (int j = 0; j < clusters.size(); j++)
	{
		// for each cluster, compute average normal direction
		Eigen::Vector3f average_normal(0.f, 0.f, 0.f);

		int cluster_size = clusters[j].indices.size();

		for (int i = 0; i < cluster_size; i++)
		{
			int index = clusters[j].indices[i];

			Eigen::Vector3f point_normal(normals->points[index].normal_x, normals->points[index].normal_y, normals->points[index].normal_z);

			// make normal point up
			if (point_normal(2) < 0) point_normal *= -1.f;
			
			average_normal += point_normal;
		}

		average_normal.normalize();

		PointCloudT::Ptr cloud(new PointCloudT);

		pcl::PointIndices::Ptr inliers(new pcl::PointIndices(clusters[j]));

		extract_indices_.setInputCloud(cloud_in);
		extract_indices_.setIndices(inliers);
		extract_indices_.setNegative(false);
		extract_indices_.filter(*cloud);

		pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_for_fe(new pcl::PointCloud<pcl::PointXYZ>);

		pcl::copyPointCloud(*cloud, *cloud_for_fe);

		pcl::MomentOfInertiaEstimation<pcl::PointXYZ> feature_extractor_;
		feature_extractor_.setInputCloud(cloud_for_fe);
		feature_extractor_.compute();

		pcl::PointXYZ min_point_AABB;
		pcl::PointXYZ max_point_AABB;
		pcl::PointXYZ min_point_OBB;
		pcl::PointXYZ max_point_OBB;
		pcl::PointXYZ position_OBB;
		Eigen::Matrix3f rotational_matrix_OBB;
		float major_value, middle_value, minor_value;
		Eigen::Vector3f major_vector, middle_vector, minor_vector;
		Eigen::Vector3f mass_center;

		feature_extractor_.getOBB(min_point_OBB, max_point_OBB, position_OBB, rotational_matrix_OBB);
		feature_extractor_.getEigenValues(major_value, middle_value, minor_value);
		feature_extractor_.getEigenVectors(major_vector, middle_vector, minor_vector);
		feature_extractor_.getMassCenter(mass_center);

		//std::cout << major_value << " " << middle_value << " " << minor_value << "\n";

		float elongation = major_value / middle_value;

		//std::cout << "elongation " << elongation << "\n\n";

		// keep long leaf
		if (elongation < 5.f)
		{
			//std::cout << "not long enough\n";
			continue;
		}

		// min point to line distance search
		float min_point_to_line_dist = 1000.f;
		int probe_idx = -1;
		for (int i = 0; i < cluster_size; i++)
		{
			int index = clusters[j].indices[i];

			Eigen::Vector3f p(cloud_in->points[index].x, cloud_in->points[index].y, cloud_in->points[index].z);
			p -= mass_center;

			float distance = (p - average_normal*p.dot(average_normal)).squaredNorm();

			if (distance < min_point_to_line_dist)
			{
				min_point_to_line_dist = distance;
				probe_idx = index;
			}
		}

		//viewer_->removeAllShapes();
		Eigen::Vector3f position(position_OBB.x, position_OBB.y, position_OBB.z);
		Eigen::Quaternionf quat(rotational_matrix_OBB);
		viewer_->addCube(position, quat, max_point_OBB.x - min_point_OBB.x, max_point_OBB.y - min_point_OBB.y, max_point_OBB.z - min_point_OBB.z, "OBB" + std::to_string(j));

		// scale the eigen vectors
		major_vector = major_vector*0.02;
		middle_vector = middle_vector*0.02;
		minor_vector = minor_vector*0.02;

		pcl::PointXYZ center(mass_center(0), mass_center(1), mass_center(2));
		pcl::PointXYZ x_axis(major_vector(0) + mass_center(0), major_vector(1) + mass_center(1), major_vector(2) + mass_center(2));
		pcl::PointXYZ y_axis(middle_vector(0) + mass_center(0), middle_vector(1) + mass_center(1), middle_vector(2) + mass_center(2));
		pcl::PointXYZ z_axis(minor_vector(0) + mass_center(0), minor_vector(1) + mass_center(1), minor_vector(2) + mass_center(2));

		average_normal *= 0.04;
		pcl::PointXYZ average_normal_axis(average_normal(0) + mass_center(0), average_normal(1) + mass_center(1), average_normal(2) + mass_center(2));
		pcl::PointXYZ probe_point(center);
		pcl::Normal probe_normal(normals->points[probe_idx].normal_x, normals->points[probe_idx].normal_y, normals->points[probe_idx].normal_z);
		
		if (probe_idx != -1)
		{
			average_normal_axis.x = average_normal(0) + cloud_in->points[probe_idx].x;
			average_normal_axis.y = average_normal(1) + cloud_in->points[probe_idx].y;
			average_normal_axis.z = average_normal(2) + cloud_in->points[probe_idx].z;

			probe_point.x = cloud_in->points[probe_idx].x;
			probe_point.y = cloud_in->points[probe_idx].y;
			probe_point.z = cloud_in->points[probe_idx].z;

			pcl::ModelCoefficients sphere; sphere.values.resize(4);
			sphere.values[0] = probe_point.x; sphere.values[1] = probe_point.y; sphere.values[2] = probe_point.z;
			sphere.values[3] = 0.002;

			viewer_->addSphere(sphere, "probe point" + std::to_string(j), 0);
		}

		//viewer_->addLine(center, x_axis, 1.0f, 0.0f, 0.0f, "major eigen vector" + std::to_string(j));
		//viewer_->addLine(center, y_axis, 0.0f, 1.0f, 0.0f, "middle eigen vector" + std::to_string(j));
		//viewer_->addLine(center, z_axis, 0.0f, 0.0f, 1.0f, "minor eigen vector" + std::to_string(j));
		viewer_->addLine(probe_point, average_normal_axis, 0.8f, 0.8f, 0.8f, "average normal" + std::to_string(j));

		viewer_->setRepresentationToWireframeForAllActors();
		//viewer_->removeAllPointClouds();
		//viewer_->addPointCloud(cloud);

		pcl::PointXYZRGBNormal pn;

		pn.x = cloud_in->points[probe_idx].x;
		pn.y = cloud_in->points[probe_idx].y;
		pn.z = cloud_in->points[probe_idx].z;
		pn.r = pn.g = pn.b = 255;
		pn.normal_x = normals->points[probe_idx].normal_x;
		pn.normal_y = normals->points[probe_idx].normal_y;
		pn.normal_z = normals->points[probe_idx].normal_z;

		probe_pn_vec.push_back(pn);
	
		//probeLeaf(cloud_in->points[probe_idx], normals->points[probe_idx]);
	}

	display();
}


void VisionArmCombo::probeLeaf(PointT & probe_point, pcl::Normal & normal)
{
	std::vector<ArmConfig> solution_config_vec_refine;
	Eigen::Matrix4d probe_hand_pose_final;
	Eigen::Vector3d hand_translation_refine;

	//std::cout << "try to probe a leaf\n";

	// try probe
	if (computeCollisionFreeProbeOrScanPose(probe_point, normal, true, solution_config_vec_refine, probe_hand_pose_final, hand_translation_refine))
	{
		//pp_.forwardKinematicsUR10(solution_config_vec_refine[0].joint_pos_f);
		// compute the hand pose with probe 10cm away from the target
		Eigen::Matrix4d final_to_prepare = Eigen::Matrix4d::Identity();
		final_to_prepare(2, 3) = -0.1;
		Eigen::Matrix4d probe_hand_pose_prepare = probe_hand_pose_final*probe_to_hand_.inverse()*final_to_prepare*probe_to_hand_;

		//std::cout << "probe_hand_pose_final\n" << probe_hand_pose_final << "\n";

		//std::cout << "probe_hand_pose_prepare\n" << probe_hand_pose_prepare << "\n";

		// visualization
		/*std::cout << "showing prepare probe pose\n";
		Eigen::Affine3f affine_pose;
		affine_pose.matrix() = probe_hand_pose_prepare.cast<float>();
		viewer_->removeAllCoordinateSystems();
		viewer_->addCoordinateSystem(0.2, affine_pose, "prepare probe pose", 0);
		display();*/

		// ik
		std::vector<int> ik_sols_vec;

		inverseKinematics(probe_hand_pose_prepare, ik_sols_vec);

		bool solution_found = false;

		float config[6];

		ArmConfig prepare_probe_config;

		for (auto idx : ik_sols_vec)
		{
			float sol_f[6];

			double2float(ik_sols_ + idx * num_joints_, sol_f);

			if (!pp_.collisionCheckForSingleConfig(sol_f))
			{
				double2float(ik_sols_ + idx * num_joints_, config);

				prepare_probe_config.setJointPos(ik_sols_[idx*num_joints_], ik_sols_[idx*num_joints_ + 1],
													ik_sols_[idx*num_joints_ + 2], ik_sols_[idx*num_joints_ + 3],
														ik_sols_[idx*num_joints_ + 4], ik_sols_[idx*num_joints_ + 5]);

				solution_found = true;
			}
		}

		if (!solution_found)
		{
			std::cout << "probe_hand_pose_prepare ik solution not found\n";
			return;
		}

		//std::cout << "try to move to prepare probe pose\n";

		if (!moveToConfigGetKinectPointCloud(prepare_probe_config, false, true))
		{
			std::cout << "could not reach prepare probe pose\n";
			return;
		}

		robot_arm_client_->moveHandJ(solution_config_vec_refine[0].joint_pos_d, move_joint_speed_, move_joint_acceleration_, true);

		display();
		
		robot_arm_client_->moveHandJ(prepare_probe_config.joint_pos_d, move_joint_speed_, move_joint_acceleration_, true);
	}
}

void VisionArmCombo::display()
{
	if (view_time_ == 0) viewer_->spin();
	else viewer_->spinOnce(view_time_);
}

void VisionArmCombo::calibrateKinectRGBCamera()
{
	if (robot_arm_client_ == NULL) initRobotArmClient();
//	if (kinect_thread_ == NULL)	initKinectThread();

//	initEXO_RGB_Cam();
//	exo_rgb_cam_->init();

	int nframes = 36;

	if (nframes % 2 != 0 || nframes < 6)
	{
		std::cout << "number of frames not even or not enough\n";
		return;
	}

	cv::Mat cameraMatrix, distCoeffs;
	std::vector<cv::Mat> image_vec;
	std::vector<Eigen::Matrix4d*> tcp_pose_vec; tcp_pose_vec.resize(nframes);
	cv::Size boardSize, imageSize;
	float squareSize, aspectRatio;

	std::vector<std::vector<cv::Point2f>> imagePoints;

	// IMPORTANT
	cv::SimpleBlobDetector::Params params;
	params.maxArea = 200 * 200;
	params.minArea = 20 * 20;
	cv::Ptr<cv::FeatureDetector> blobDetect = cv::SimpleBlobDetector::create(params);

#if 0
	imageSize.width = kinect_thread_->cColorWidth;
	imageSize.height = kinect_thread_->cColorHeight;
#else
	imageSize.width = 1920;
	imageSize.height = 1200;
#endif

	boardSize.width = 4;
	boardSize.height = 11;

	squareSize = 0.02f;

	for (int i = 0; i < nframes; i++)
	{
		cv::Mat view, viewGray;

		std::vector<cv::Point2f> pointbuf;

		while (true)
		{
#if 0
			view = kinect_thread_->getCurRGB();
#else
			exo_rgb_cam_->acquireRGBImage();
			view = exo_rgb_cam_->rgb_frame;
#endif

			cv::cvtColor(view, viewGray, CV_BGR2GRAY);

			pointbuf.clear();

			// ASYMMETRIC CIRCLE GRID PATTERN
			bool found = findCirclesGrid(view, boardSize, pointbuf, cv::CALIB_CB_ASYMMETRIC_GRID | cv::CALIB_CB_CLUSTERING, blobDetect);

			cv::Mat view_copy;
			
			view.copyTo(view_copy);

			if (found)
			{
				cv::drawChessboardCorners(view_copy, boardSize, cv::Mat(pointbuf), found);
				cv::circle(view_copy, pointbuf[0], 30, cv::Scalar(0, 255, 0), 4);
				cv::circle(view_copy, pointbuf[1], 30, cv::Scalar(0, 0, 255), 4);
			}

			cv::Mat shrinked;

			cv::resize(view_copy, shrinked, cv::Size(), 0.5, 0.5);

			cv::imshow("rgb", shrinked);

			int key = cv::waitKey(1);

			//hit space
			if (key == 32)	break;
		}

		std::cout << "Image " << i << " done\n";

		cv::Mat view_save;

		view.copyTo(view_save);

		image_vec.push_back(view_save);

		imagePoints.push_back(pointbuf);

		double array6[6];

		robot_arm_client_->getCartesianInfo(array6);

		Eigen::Matrix4d* tcp_pose = new Eigen::Matrix4d;

		array6ToEigenMat4d(array6, *tcp_pose);

		std::cout << *tcp_pose << "\n";

		tcp_pose_vec[i] = tcp_pose;
	}

	cameraMatrix = cv::Mat::eye(3, 3, CV_64F);

	distCoeffs = cv::Mat::zeros(8, 1, CV_64F);

	std::vector<std::vector<cv::Point3f>> objectPoints(1);

	std::vector<cv::Point3f> corners;

	for (int i = 0; i < boardSize.height; i++)
	{
		for (int j = 0; j < boardSize.width; j++)
		{
			corners.push_back(cv::Point3f(float((2 * j + i % 2)*squareSize), float(i*squareSize), 0));
		}
	}

	objectPoints[0] = corners;

	objectPoints.resize(imagePoints.size(), objectPoints[0]);

	std::vector<cv::Mat> camera_rotation_vec, camera_translation_vec;	//camera to calibration pattern

	double rms = cv::calibrateCamera(objectPoints, imagePoints, imageSize, cameraMatrix, distCoeffs, camera_rotation_vec, camera_translation_vec, 0);

	printf("RMS error reported by calibrateCamera: %g\n", rms);

	/*for (int i = 0; i < image_vec.size(); i++)
	{
		std::cout << "rotation\n" << camera_rotation_vec[i] << "\ntranslation\n" << camera_translation_vec[i]<<"\n";
		cv::imshow("image", image_vec[i]);
		cv::waitKey(0);
	}*/

	bool ok = cv::checkRange(cameraMatrix) && cv::checkRange(distCoeffs);

	cv::FileStorage fs("kinectRGBCalibration.yml", cv::FileStorage::WRITE);

	fs << "camera_matrix" << cameraMatrix;
	fs << "distortion_coefficients" << distCoeffs;
	fs << "nframes" << nframes;
	fs << "camera poses" << "[";
	for (int i = 0; i < nframes; i++)
	{
		cv::Mat rotation;
		cv::Rodrigues(camera_rotation_vec[i], rotation);
		cv::Mat transform = cv::Mat::eye(4, 4,CV_64F);
		cv::Mat sub = transform(cv::Rect(0, 0, 3, 3));
		rotation.copyTo(sub);
		sub = transform(cv::Rect(3, 0, 1, 3));
		camera_translation_vec[i].copyTo(sub);

		fs << transform;
	}
	fs << "]";

	fs << "TCP poses" << "[";
	for (int i = 0; i < nframes; i++)
	{
		cv::Mat tcp_pose(4, 4, CV_64F);
		for (int y = 0; y < 4; y++)
			for (int x = 0; x < 4; x++)
				tcp_pose.at<double>(y, x) = (*tcp_pose_vec[i])(y, x);

		fs << tcp_pose;
	}
	fs << "]";

	fs.release();
}

/*
	robot sensor calibration solving ax=xb on euclidean group 1994
*/
void VisionArmCombo::KinectRGBHandEyeCalibration()
{
	cv::FileStorage fs("kinectRGBCalibration.yml", cv::FileStorage::READ);
	cv::FileNode camera_poses = fs["camera poses"];
	cv::FileNode tcp_poses = fs["TCP poses"];

	int nframes;

	fs["nframes"] >> nframes;

	std::vector<Eigen::Matrix4d*> camera_pose_vec; 
	std::vector<Eigen::Matrix4d*> tcp_pose_vec;	

	// iterate through a sequence using FileNodeIterator
	for (cv::FileNodeIterator it = camera_poses.begin(); it != camera_poses.end(); ++it)
	{
		cv::Mat camera_pose;
		(*it) >> camera_pose;

		Eigen::Matrix4d* transform = new Eigen::Matrix4d;

		for (int y = 0; y < 4; y++)
			for (int x = 0; x < 4; x++)
				(*transform)(y, x) = camera_pose.at<double>(y, x);

		camera_pose_vec.push_back(transform);
	}

	for (cv::FileNodeIterator it = tcp_poses.begin(); it != tcp_poses.end(); ++it)
	{
		cv::Mat tcp_pose;
		(*it) >> tcp_pose;

		Eigen::Matrix4d* transform = new Eigen::Matrix4d;

		for (int y = 0; y < 4; y++)
			for (int x = 0; x < 4; x++)
				(*transform)(y, x) = tcp_pose.at<double>(y, x);

		tcp_pose_vec.push_back(transform);
	}

	fs.release();

	// hand eye calibration
	Eigen::Matrix3d M = Eigen::Matrix3d::Zero();
	Eigen::MatrixXd C(3 * nframes *(nframes-1), 3);
	Eigen::VectorXd d(3 * nframes * (nframes-1));
	Eigen::VectorXd bA(3 * nframes * (nframes-1));
	Eigen::VectorXd bB(3 * nframes * (nframes-1));

	int count = 0;

	for (int i = 0; i < nframes; i++)
	{
		for (int j = 0; j < nframes; j++)
		{
			if (i == j) continue;

			// TCP pose motion
			Eigen::Matrix4d A;
			A = (*tcp_pose_vec[i]).inverse() * (*tcp_pose_vec[j]);	//base to robot hand

			// camera pose motion
			Eigen::Matrix4d B;
			B = (*camera_pose_vec[i])*(*camera_pose_vec[j]).inverse();	//camera to calibration board

			//log Rotation
			Eigen::Matrix3d alpha, beta;

			double theta = acos(0.5*(A.block<3, 3>(0, 0).trace() - 1.));

			alpha = theta*0.5 / sin(theta)*(A.block<3, 3>(0, 0) - A.block<3, 3>(0, 0).transpose());

			theta = acos(0.5*(B.block<3, 3>(0, 0).trace() - 1.));

			beta = theta*0.5 / sin(theta)*(B.block<3, 3>(0, 0) - B.block<3, 3>(0, 0).transpose());

			M = M + beta*alpha.transpose();

			C.block<3, 3>(count * 3, 0) = Eigen::Matrix3d::Identity() - A.block<3, 3>(0, 0);
			bA.block<3, 1>(count * 3, 0) = A.block<3, 1>(0, 3);
			bB.block<3, 1>(count * 3, 0) = B.block<3, 1>(0, 3);
			count++;
		}
	}

	Eigen::EigenSolver<Eigen::Matrix3d> es(M.transpose()*M);

	Eigen::Matrix3d lambda;

	lambda = es.eigenvalues().real().cwiseSqrt().cwiseInverse().asDiagonal();
	
	Eigen::Matrix3d hand_to_eye_rotation = es.eigenvectors().real()*lambda*es.eigenvectors().real().inverse()*M.transpose();

	for (int i = 0; i < nframes*(nframes - 1); i++)
		bB.block<3, 1>(i * 3, 0) = hand_to_eye_rotation*bB.block<3, 1>(i * 3, 0);

	d = bA - bB;

	Eigen::Vector3d hand_to_eye_translation = (C.transpose()*C).inverse()*C.transpose()*d;

	cv::Mat hand_to_eye = cv::Mat::eye(4, 4, CV_64F);

	for (int y = 0; y < 3; y++)
		for (int x = 0; x < 3; x++)
			hand_to_eye.at<double>(y, x) = hand_to_eye_rotation(y, x);

	for (int i = 0; i < 3; i++)
		hand_to_eye.at<double>(i, 3) = hand_to_eye_translation(i);

	std::cout << "hand to eye\n" << hand_to_eye << "\n";

	cv::FileStorage fs1("kinectRGBHandEyeCalibration.yml", cv::FileStorage::WRITE);

	fs1 << "hand to eye" << hand_to_eye;

	fs1.release();
}

void VisionArmCombo::markerDetection()
{
	cv::Mat markerImage;
	//make pot grid with marker 
	int marker_img_size = 1200;	//multiple of 5+2
	int grid_width = 7;
	int grid_height = 5;

	//cv::aruco::drawMarker(marker_dictionary_, 0, marker_img_size, markerImage, 1);

	cv::Mat center_square;
	center_square.create(marker_img_size, marker_img_size, CV_8UC3);

	std::memset(center_square.ptr(), 255, marker_img_size * marker_img_size * 3);

	float square_size = marker_img_size/4.f*3.5f;
	cv::rectangle(center_square, cv::Point2f((marker_img_size - square_size)*0.5f, (marker_img_size - square_size)*0.5f),
					cv::Point2f(marker_img_size-(marker_img_size - square_size)*0.5f, marker_img_size - (marker_img_size - square_size)*0.5f), cv::Scalar(0,0,0),4);

	cv::Mat block;
	block.create(marker_img_size, marker_img_size, CV_8UC3);
	std::memset(block.ptr(), 255, marker_img_size * marker_img_size * 3);
	
	cv::circle(block, cv::Point2f(marker_img_size*0.5f, marker_img_size*0.5f), 780/2/*marker_img_size / 3*/, cv::Scalar(0, 0, 0), 4);

	cv::circle(block, cv::Point2f(marker_img_size*0.5f, marker_img_size*0.5f), 40, cv::Scalar(0, 0, 0), 40);

	//cv::imshow("block", block);

	//cv::waitKey(0);

	cv::Mat block_grid;

	block_grid.create(marker_img_size * grid_height, marker_img_size * grid_width, CV_8UC3);

	for (int y = 0; y < grid_height; y++)
	{
		for (int x = 0; x < grid_width; x++)
		{
			block.copyTo(block_grid(cv::Rect(x*marker_img_size, y*marker_img_size, marker_img_size, marker_img_size)));
		}
	}

	//cv::Mat marker_img;
	//cv::cvtColor(markerImage, marker_img, CV_GRAY2BGR);
	//marker_img.copyTo(block_grid(cv::Rect((grid_width/2)*marker_img_size, (grid_height/2)*marker_img_size, marker_img_size, marker_img_size)));
	center_square.copyTo(block_grid(cv::Rect((grid_width / 2)*marker_img_size, (grid_height / 2)*marker_img_size, marker_img_size, marker_img_size)));

	cv::imwrite("block_grid.png", block_grid);

	cv::Mat shrink;
	cv::resize(block_grid, shrink, cv::Size(), 0.1, 0.1);

	cv::imshow("block gird", shrink);
	cv::waitKey(0);

	return;
	

	/*	//generate marker images and save
	for (int i = 0; i < 50; i++)
	{
		cv::aruco::drawMarker(marker_dictionary_, i, 700, markerImage, 1);

		cv::imshow("marker", markerImage);

		cv::imwrite("Markers\\marker_" + std::to_string(i) + ".png", markerImage);

		cv::waitKey(100);
	}*/

	if (robot_arm_client_ == NULL) initRobotArmClient();

	if (kinect_thread_ == NULL)	initKinectThread();

	cv::Mat rgb;
	cv::Vec3d rot;
	cv::Vec3d tran;

	ArmConfig config;
	config.setJointPos(-86.5, -99.37, -155.46, -12.91, 90., -178.94);
	config.toRad();

	robot_arm_client_->moveHandJ(config.joint_pos_d, 0.1, 0.1, true);


	while (true)
	{
		rgb = kinect_thread_->getCurRGB();

		std::vector<int> markerIds; 
		std::vector<std::vector<cv::Point2f>> markerCorners, rejectedCandidates;
		cv::aruco::detectMarkers(rgb, marker_dictionary_, markerCorners, markerIds, detector_params_, rejectedCandidates);
		
		std::vector<cv::Vec3d> rvecs, tvecs;

		cv::aruco::estimatePoseSingleMarkers(markerCorners, marker_length_, kinect_rgb_camera_matrix_cv_, kinect_rgb_dist_coeffs_cv_, rvecs, tvecs);

		cv::aruco::drawDetectedMarkers(rgb, markerCorners, markerIds);

		for (unsigned int i = 0; i < markerIds.size(); i++)
		{
			cv::aruco::drawAxis(rgb, kinect_rgb_camera_matrix_cv_, kinect_rgb_dist_coeffs_cv_, rvecs[i], tvecs[i], marker_length_*0.5f);
			std::cout <<"id "<< i<<" rot " << rvecs[i] << " tran " << tvecs[i]<<"\n";
			rot = rvecs[i];
			tran = tvecs[i];
		}

		cv::imshow("marker", rgb);

		int key = cv::waitKey(10);

		if (key == 113)	//q
		{
			break;
		}
	}

	cv::Mat rgb_to_marker_rot_cv;

	cv::Rodrigues(rot, rgb_to_marker_rot_cv);

	for (int y = 0; y < 3; y++)
		for (int x = 0; x < 3; x++)
			cur_rgb_to_marker_(y, x) = rgb_to_marker_rot_cv.at<double>(y, x);

	for (int y = 0; y < 3; y++) cur_rgb_to_marker_(y, 3) = tran[y];

	std::cout << "rgb to marker:\n" << cur_rgb_to_marker_ << "\n";

	Eigen::Matrix4d base_to_hand;

	getCurHandPoseD(base_to_hand);

	Eigen::Matrix3d  rotate_x_pi;

	rotate_x_pi = Eigen::AngleAxisd(M_PI, Eigen::Vector3d::UnitX());

	Eigen::Matrix4d marker_to_gripper = Eigen::Matrix4d::Identity();

	marker_to_gripper.topLeftCorner<3, 3>() = rotate_x_pi.matrix();

	

	Eigen::Matrix4d base_to_marker = base_to_hand*hand_to_rgb_*cur_rgb_to_marker_;

	std::cout << "base to marker:\n" << base_to_marker << "\n";

	for (int y = 2; y >= -2; y--)
	{
		for (int x = -3; x <= 3; x++)
		{
			std::getchar();

			Eigen::Matrix4d marker_to_gripper_translate = Eigen::Matrix4d::Identity();
			marker_to_gripper_translate.col(3).head(3) << x* marker_length_, y* marker_length_, -0.03;

			Eigen::Matrix4d probe_pose_eigen = base_to_marker*marker_to_gripper*marker_to_gripper_translate*probe_to_hand_;

			std::cout << "probe pose:\n" << probe_pose_eigen << "\n";

			// move arm
			double pose[6];

			eigenMat4dToArray6(probe_pose_eigen, pose);

			robot_arm_client_->moveHandL(pose, 0.1, 0.1);
		}
	}

}

void VisionArmCombo::cvTransformToEigenTransform(cv::Mat & cv_transform, Eigen::Matrix4d & eigen_transform)
{
	for (int y = 0; y < 4; y++)
		for (int x = 0; x < 4; x++)
			eigen_transform(y, x) = cv_transform.at<double>(y, x);
}

bool VisionArmCombo::scanSphereOnTable(Eigen::Vector3d & hand_pos, Eigen::Vector3d & sphere_pos, int side)
{
	if (robot_arm_client_ == NULL) initRobotArmClient();
	if (line_profiler_ == NULL) initLineProfiler();

	double pose[6];
	double tran[3];

	Eigen::Matrix4f cur_pose;
	getCurHandPose(cur_pose);
	if (cur_pose(2, 2) >= 0.)	//check z direction
	{
		std::cout << "gripper pointing down!\n";
		return false;
	}

	double sign;
	if (side == 0) sign = 1.0;
	else if (side == 1) sign = -1.0;
	else std::cout << "wrong side\n";

	PointCloudT::Ptr scan_cloud(new PointCloudT);

	Eigen::Matrix4d pose_eigen = Eigen::Matrix4d::Zero();

	pose_eigen(1, 0) = -sign;
	pose_eigen(0, 1) = -sign;
	pose_eigen(2, 2) = -1.;
	pose_eigen(3, 3) = 1.;

	pose_eigen(0, 3) = hand_pos(0);
	pose_eigen(1, 3) = hand_pos(1) + sign*0.075;
	pose_eigen(2, 3) = hand_pos(2);

	eigenMat4dToArray6(pose_eigen, pose);

	float scan_vec = 0.2;
	//pose[0] = hand_pos(0); pose[1] = hand_pos(1); pose[2] = hand_pos(2);
	//pose[3] = rot_vec[0], pose[4] = rot_vec[1], pose[5] = rot_vec[2];

	tran[0] = 0.;
	tran[1] = scan_vec;
	tran[2] = 0.;

	robot_arm_client_->moveHandL(pose, 0.2, 0.2);
	robot_arm_client_->waitTillHandReachDstPose(pose);

	Sleep(1000);
	/*std::cout << "Ready\n";
	std::getchar();*/

	getCurHandPose(cur_pose);

	line_profiler_->m_vecProfileData.clear();
	scanTranslateOnly(tran, scan_cloud, scan_acceleration_, scan_speed_);

	pcl::transformPointCloud(*scan_cloud, *scan_cloud, cur_pose*handToScanner_);

	//viewer_->addPointCloud(scan_cloud, "sphere_cloud" + std::to_string(GetTickCount()), 0); viewer_->spin();

	PointCloudT::Ptr tmp_cloud(new PointCloudT);
	pass_.setInputCloud(scan_cloud);
	pass_.setFilterFieldName("z");
	pass_.setFilterLimits(0.12, 0.25);
	pass_.setNegative(false);
	pass_.filter(*tmp_cloud);

	//viewer_->addPointCloud(tmp_cloud, "sphere_cloud" + std::to_string(GetTickCount()), 0); viewer_->spin();

	pcl::ModelCoefficients coefficients;
	pcl::PointIndices::Ptr inliers(new pcl::PointIndices());
	pcl::SACSegmentation<PointT> seg;
	seg.setOptimizeCoefficients(true);
	seg.setModelType(pcl::SACMODEL_SPHERE); //detecting SPHERE
	seg.setMethodType(pcl::SAC_RANSAC);
	seg.setDistanceThreshold(0.0005);
	seg.setRadiusLimits(0.012, 0.013);
	seg.setMaxIterations(100000);
	seg.setInputCloud(tmp_cloud);
	seg.segment(*inliers, coefficients);

	if (coefficients.values.size() == 4)
	{
		std::cout << coefficients << "\n";
		viewer_->addSphere(coefficients, "sphere" + std::to_string(GetTickCount()), 0);
		sphere_pos(0) = coefficients.values[0];
		sphere_pos(1) = coefficients.values[1];
		sphere_pos(2) = coefficients.values[2];
	}

	pcl::ExtractIndices<PointT> extract;
	extract.setInputCloud(tmp_cloud);
	extract.setIndices(inliers);
	extract.setNegative(false);
	extract.filter(*scan_cloud);
	
	viewer_->addPointCloud(scan_cloud, "sphere_cloud" + std::to_string(GetTickCount()), 0);
	//viewer_->spin();

	return true;
}

void VisionArmCombo::scanAndProbeTest()
{
	if (robot_arm_client_ == NULL) initRobotArmClient();
	if (line_profiler_ == NULL) initLineProfiler();

	// first ball
	Eigen::Vector3d hand_pos(0.212, -0.360, 0.332);
	Eigen::Vector3d sphere_pos1, sphere_pos2, sphere_pos3;
	scanSphereOnTable(hand_pos, sphere_pos1);

	// second ball the other side
	hand_pos(0) = 0.928; hand_pos(1) = -0.042;
	scanSphereOnTable(hand_pos, sphere_pos2);

	hand_pos(0) = 0.22; hand_pos(1) = 0.250;
	scanSphereOnTable(hand_pos, sphere_pos3);

	std::cout << "1-3: " << (sphere_pos1 - sphere_pos3).norm() << "\n";
	std::cout << "1-2: " << (sphere_pos1 - sphere_pos2).norm() << "\n";
	std::cout << "3-2: " << (sphere_pos3 - sphere_pos2).norm() << "\n";
	//----------------------------------------------- old code for square 


	double pose[6];
	double tran[3];
	
	Eigen::Matrix4f cur_pose;
	getCurHandPose(cur_pose);
	if (cur_pose(2, 2) >= 0.)	//check z direction
	{
		std::cout << "gripper pointing down!\n";
		return;
	}

	PointCloudT::Ptr scan_cloud(new PointCloudT);
	PointCloudT::Ptr scan_cloud_reverse(new PointCloudT);

	Eigen::AngleAxisd scan_rot(Eigen::AngleAxisd(M_PI, Eigen::Vector3d::UnitX())*Eigen::AngleAxisd(-M_PI*0.5, Eigen::Vector3d::UnitZ()));
	Eigen::Vector3d rot_vec(scan_rot.angle()*scan_rot.axis());


	float scan_vec = 0.3;
	pose[0] = 0.62; pose[1] = 0.255-0.3; pose[2] = 0.372;
	pose[3] = rot_vec[0], pose[4] = rot_vec[1], pose[5] = rot_vec[2];
	
	tran[0] = 0.;
	tran[1] = scan_vec;
	tran[2] = 0.;

	robot_arm_client_->moveHandL(pose, 0.2, 0.2);
	robot_arm_client_->waitTillHandReachDstPose(pose);

	Sleep(1000);
	/*std::cout << "Ready\n";
	std::getchar();*/

	getCurHandPose(cur_pose);

	line_profiler_->m_vecProfileData.clear();
	scanTranslateOnly(tran, scan_cloud, scan_acceleration_, scan_speed_);

	vox_.setInputCloud(scan_cloud);
	vox_.setLeafSize(0.0002, 0.0002, 0.0002);

	PointCloudT::Ptr scan_cloud_down(new PointCloudT);

	vox_.filter(*scan_cloud_down);

	PointCloudT::Ptr scan_cloud_down_base(new PointCloudT);

	pcl::transformPointCloud(*scan_cloud_down, *scan_cloud_down_base, cur_pose*handToScanner_);

	PointCloudT::Ptr tmp_cloud(new PointCloudT);
	pass_.setInputCloud(scan_cloud);
	pass_.setFilterFieldName("z");
	pass_.setFilterLimits(0, 0.25);
	pass_.setNegative(false);
	pass_.filter(*tmp_cloud);

	pcl::ModelCoefficients coefficients;
	pcl::PointIndices inliers;
	pcl::SACSegmentation<PointT> seg;
	seg.setOptimizeCoefficients(true);
	seg.setModelType(pcl::SACMODEL_SPHERE); //detecting SPHERE
	seg.setMethodType(pcl::SAC_RANSAC);
	seg.setDistanceThreshold(0.0005);
	seg.setRadiusLimits(0.012, 0.013);
	seg.setMaxIterations(100000);
	seg.setInputCloud(tmp_cloud);
	seg.segment(inliers, coefficients);

	std::cout << coefficients << "\n";

	viewer_->addSphere(coefficients, "sphere", 0);

	viewer_->addPointCloud(tmp_cloud, "tmp_cloud", 0);
	viewer_->spin();

	//std::cout << "Ready\n";
	//std::getchar();

	/*robot_arm_client_->getCartesianInfo(pose);
	pose[3] = 3.034, pose[4] = 0.0267, pose[5] = 0.1917;
	robot_arm_client_->moveHandL(pose, 0.5, 0.5);
	robot_arm_client_->waitTillHandReachDstPose(pose);*/

	/*Sleep(1000);

	std::cout << "\n";

	tran[0] = -scan_vec;
	PointCloudT::Ptr scan_cloud1(new PointCloudT);

	Eigen::Matrix4f cur_pose1;
	getCurHandPose(cur_pose1);

	line_profiler_->m_vecProfileData.clear();
	scanTranslateOnly(tran, scan_cloud1, scan_acceleration_, scan_speed_);

	vox_.setInputCloud(scan_cloud1);
	vox_.setLeafSize(0.0002, 0.0002, 0.0002);

	PointCloudT::Ptr scan_cloud_down1(new PointCloudT);

	vox_.filter(*scan_cloud_down1);

	PointCloudT::Ptr scan_cloud_down_base1(new PointCloudT);

	pcl::transformPointCloud(*scan_cloud_down1, *scan_cloud_down_base1, cur_pose1*handToScanner_);

	for (int i = 0; i < scan_cloud_down_base1->points.size(); i++)
	{
		scan_cloud_down_base1->points[i].b = 0;
	}

	*scan_cloud_down_base1 += *scan_cloud_down_base;

	vox_.setInputCloud(scan_cloud_down_base1);

	vox_.filter(*scan_cloud_down_base);*/


	//viewer_->addPointCloud(scan_cloud_down_base, "0");
	//viewer_->addPointCloud(scan_cloud_down_base1, "1");
	//viewer_->spin();

	//test planes
	/*PointCloudT::Ptr metal(new PointCloudT);
	PointCloudT::Ptr table (new PointCloudT);
	pass_.setInputCloud(scan_cloud_down_base);
	pass_.setFilterFieldName("z");
	pass_.setFilterLimits(-0.1598f, -0.1582f);
	pass_.setFilterLimitsNegative(false);
	pass_.filter(*metal);

	pass_.setFilterLimits(-0.25f, -0.194f);
	pass_.filter(*table);

	for (auto &p : metal->points)
	{
		p.b = 0;
	}

	pcl::PCA<PointT> pca(*metal);

	Eigen::Matrix3d eigen_vectors = pca.getEigenVectors().cast<double>();

	std::cout << "metal:\n" << eigen_vectors<<"\n";

	pca.setInputCloud(table);

	Eigen::Matrix3d table_eigen_vectors = pca.getEigenVectors().cast<double>();

	std::cout << "table:\n" << table_eigen_vectors << "\n";

	if(eigen_vectors.col(2)(2) *table_eigen_vectors.col(2)(2) < 0.)
		std::cout<<"angle: "<<acos(eigen_vectors.col(2).dot(-table_eigen_vectors.col(2)))/M_PI*180.<<"\n";
	else
		std::cout << "angle: " << acos(eigen_vectors.col(2).dot(table_eigen_vectors.col(2))) / M_PI*180. << "\n";

	viewer_->addPointCloud(table, "0");
	viewer_->addPointCloud(metal, "1");
	viewer_->spin(); */


	//fitPotRing(scan_cloud_down_base);

	//find3DMarker(scan_cloud_down_base);

	robot_arm_client_->rotateJointRelative(4, -180., 0.7, 0.7);

#if 0
	pose[0] = 0.862;  pose[1] = 0.049; pose[2] = 0.255;
	robot_arm_client_->moveHandL(pose, 0.3, 0.3);
	robot_arm_client_->waitTillHandReachDstPose(pose);

	Sleep(1000);
	
	float leaf_size = 0.001;
	PointCloudT::Ptr scan_cloud2(new PointCloudT);

	getCurHandPose(cur_pose);

	tran[1] = -0.2;

	line_profiler_->m_vecProfileData.clear();
	scanTranslateOnly(tran, scan_cloud2, scan_acceleration_, scan_speed_);

	vox_.setInputCloud(scan_cloud2);
	vox_.setLeafSize(leaf_size, leaf_size, leaf_size);

	PointCloudT::Ptr scan_cloud_down2(new PointCloudT);

	vox_.filter(*scan_cloud_down2);

	PointCloudT::Ptr scan_cloud_down_base2(new PointCloudT);

	pcl::transformPointCloud(*scan_cloud_down2, *scan_cloud_down_base2, cur_pose*handToScanner_);

	*scan_cloud_down_base += *scan_cloud_down_base2;


	pose[0] = 0.862;  pose[1] = 0.373; pose[2] = 0.255;
	robot_arm_client_->moveHandL(pose, 0.3, 0.3);
	robot_arm_client_->waitTillHandReachDstPose(pose);

	Sleep(1000);

	PointCloudT::Ptr scan_cloud3(new PointCloudT);

	getCurHandPose(cur_pose);

	line_profiler_->m_vecProfileData.clear();
	scanTranslateOnly(tran, scan_cloud3, scan_acceleration_, scan_speed_);

	vox_.setInputCloud(scan_cloud3);
	vox_.setLeafSize(leaf_size, leaf_size, leaf_size);

	PointCloudT::Ptr scan_cloud_down3(new PointCloudT);

	vox_.filter(*scan_cloud_down3);

	PointCloudT::Ptr scan_cloud_down_base3(new PointCloudT);

	pcl::transformPointCloud(*scan_cloud_down3, *scan_cloud_down_base3, cur_pose*handToScanner_);

	*scan_cloud_down_base += *scan_cloud_down_base3;

	pose[0] = 0.373;  pose[1] = 0.373; pose[2] = 0.255;
	robot_arm_client_->moveHandL(pose, 0.3, 0.3);
	robot_arm_client_->waitTillHandReachDstPose(pose);

	Sleep(1000);

	PointCloudT::Ptr scan_cloud4(new PointCloudT);

	getCurHandPose(cur_pose);


	line_profiler_->m_vecProfileData.clear();
	scanTranslateOnly(tran, scan_cloud4, scan_acceleration_, scan_speed_);

	vox_.setInputCloud(scan_cloud4);
	vox_.setLeafSize(leaf_size, leaf_size, leaf_size);

	PointCloudT::Ptr scan_cloud_down4(new PointCloudT);

	vox_.filter(*scan_cloud_down4);

	PointCloudT::Ptr scan_cloud_down_base4(new PointCloudT);

	pcl::transformPointCloud(*scan_cloud_down4, *scan_cloud_down_base4, cur_pose*handToScanner_);

	*scan_cloud_down_base += *scan_cloud_down_base4;
#endif

	probeScannedSceneTest(scan_cloud_down_base);
}

int VisionArmCombo::sendRoboteqVar(int id, int value)
{
	int status = motor_controller_.SetCommand(_VAR, id, value);

	// send success, but status != RQ_SUCCESS, roboteq bug
	if ( status != RQ_SUCCESS)
	{
		cout << "set Roboteq VAR failed --> " << status << endl;
		return -1;
	}

	sleepms(1000);

	int result = -1;
	status = motor_controller_.GetValue(_VAR, id, result);

	if (status != RQ_SUCCESS)
	{
		cout << "get var failed --> " << status << endl;
	}

	std::cout << "result: " << result << "\n";

	return 0;
}

void VisionArmCombo::fitPotRing(PointCloudT::Ptr pot_cloud)
{
	PointCloudT::Ptr pot_ring_cloud(new PointCloudT);

	float radius = 0.04345f;

	for (float span = -0.001f; span <=0.001f; span += 0.001f)
	{
		for (float deg = 0; deg < 360.f; deg += 0.5f)
		{
			PointT p;
			float theta = deg / 180.*M_PI;
			p.x = (radius+span)*cos(theta);
			p.y = (radius+span)*sin(theta);
			p.z = 0.f;
			p.r = 255;
			p.g = 0;
			p.b = 0;
			pot_ring_cloud->push_back(p);
		}
	}

	PointCloudT::Ptr cropped_pot(new PointCloudT);
	pass_.setInputCloud(pot_cloud);
	pass_.setFilterFieldName("z");
	pass_.setFilterLimits(-0.02f, 0.02f);
	pass_.setFilterLimitsNegative(false);
	pass_.filter(*cropped_pot);

	Eigen::Matrix4f base_to_pot = Eigen::Matrix4f::Identity();
	base_to_pot.col(3).head(3) << -0.02, -0.49, 0.02;
	pcl::transformPointCloud(*pot_ring_cloud, *pot_ring_cloud, base_to_pot);

	pcl::IterativeClosestPoint<PointT, PointT> icp;
	icp.setInputSource(pot_ring_cloud);
	icp.setInputTarget(cropped_pot);
	PointCloudT::Ptr Final(new PointCloudT);
	icp.align(*Final);

	std::cout << icp.getFinalTransformation() << std::endl;

	viewer_->addPointCloud(Final, "final", 0);
	//viewer_->addPointCloud(pot_ring_cloud, "pot_ring_cloud", 0);
	viewer_->addPointCloud(cropped_pot, "cropped_pot", 0);
	viewer_->spin();
}

void VisionArmCombo::find3DMarker(PointCloudT::Ptr marker_cloud)
{
	// rectangle
	float marker_width = 0.0889;
	float marker_len = 0.0889;

	PointCloudT::Ptr pot_centers(new PointCloudT);
	for (int y = -2; y <= 2; y++)
	{
		for (int x = -3; x <= 3; x++)
		{
			PointT p;
			p.x = x*marker_length_ /*+ marker_width*0.5*/;
			p.y = y*marker_length_ /*+ marker_width*0.5*/;
			p.z = -0.04445f;
			p.r = 0;
			p.g = 255;
			p.b = 0;
			pot_centers->push_back(p);
		}
	}

	PointCloudT::Ptr marker_template(new PointCloudT);
	PointCloudT::Ptr marker_template_copy(new PointCloudT);


	//	pyramid
	const float top_square_radius = (1.25-0.01)*0.0254*0.5;
	const float edge_len = 1.6*0.0254;

	// top square
	for (float x = -top_square_radius; x <= top_square_radius; x += 0.001f)
	{
		for (float y = -top_square_radius; y <= top_square_radius; y += 0.001f)
		{
			PointT p;
			p.x = x;
			p.y = y;
			p.z = 0;
			p.g = 255;
			p.b = p.r = 0;
			marker_template->push_back(p);
			//p.y = -top_square_radius;
			//marker_template->push_back(p);
		}

		float y = top_square_radius;
		PointT p;
		p.x = x;
		p.y = y;
		p.z = 0;
		p.g = 255;
		p.b = p.r = 0;
		marker_template->push_back(p);
	}

	for (float y = -top_square_radius; y <= top_square_radius; y += 0.001f)
	{
		PointT p;
		p.y = y;
		p.x = top_square_radius;
		p.z = 0;
		p.g = 255;
		p.r = p.b = 0;
		marker_template->push_back(p);
		//p.x = -top_square_radius;
		//marker_template->push_back(p);
	}
	
	//four 45 deg edges
	Eigen::Vector3f edge_dir(1.0f, 1.0f, 1.0f);
	edge_dir *= -1.0f;
	edge_dir.normalize();
	Eigen::Vector3f start_point(-top_square_radius, -top_square_radius, 0);

	std::vector<std::vector<Eigen::Vector3f>> four_edges;
	

	for (int e = 0; e < 4; e++)
	{
		std::vector<Eigen::Vector3f> edge;

		if (e == 1 || e == 3)
		{
			start_point(0) *= -1.0f;
			edge_dir(0) *= -1.0f;
		}
		else if (e == 2)
		{
			start_point(1) *= -1.0f;
			edge_dir(1) *= -1.0f;
		}

		for (float len = 0; len <= edge_len; len += 0.001f)
		{
			Eigen::Vector3f ep = start_point + edge_dir*len;
			/*PointT p;
			p.x = ep(0); p.y = ep(1); p.z = ep(2);
			p.g = 255;
			p.r = p.b = 0;
			marker_template->push_back(p);*/
			edge.push_back(ep);
		}

		four_edges.push_back(edge);
	}
	const int edge_size = four_edges[0].size();

	for (int i = 0; i < edge_size; i++)
	{
		for (float a = 0.0f; a <= 1.0f; a += 0.01f)
		{
			for (int e = 0; e < 4; e++)
			{
				Eigen::Vector3f tmp = a*four_edges[e][i] + (1.0f - a)*four_edges[(e+1)%4][i];
				PointT p;
				p.x = tmp(0); p.y = tmp(1); p.z = tmp(2);
				p.g = 255;
				p.r = p.b = 0;
				marker_template->push_back(p);
			}
		}
	}

	//viewer_->addPointCloud(marker_template, "template", 0);
	//viewer_->spin();


	
/*	for (float step = 0.f; step <= 0.000; step += 0.0001f)
	{
		for (float x = 0; x <= marker_width; x += 0.0001f)
		{
			PointT p;
			p.x = x;
			p.y = 0+step;
			p.z = 0;
			p.r = 255;
			p.g = p.b = 0;
			marker_template->push_back(p);
			p.y = marker_len-step;
			marker_template->push_back(p);
		}

		for (float y = 0; y <= marker_len; y += 0.0001f)
		{
			PointT p;
			p.x = 0+step;
			p.y = y;
			p.z = 0;
			p.r = 255;
			p.g = p.b = 0;
			marker_template->push_back(p);
			p.x = marker_width-step;
			marker_template->push_back(p);
		}

		PointT p;
		p.x = marker_width;
		p.y = marker_len;
		p.z = 0;
		p.r = 255;
		p.g = p.b = 0;
		marker_template->push_back(p);
	}*/

	*marker_template_copy += *marker_template;

	PointCloudT::Ptr cropped_cloud(new PointCloudT);
	pass_.setInputCloud(marker_cloud);
	pass_.setFilterFieldName("z");
	//pass_.setFilterLimits(-0.16f, -0.14f);	//block
	pass_.setFilterLimits(-0.18f, -0.14f);	//pyramid
	pass_.setFilterLimitsNegative(false);
	pass_.filter(*cropped_cloud);

	PointCloudT::Ptr tmp_cloud(new PointCloudT);
	smallClusterRemoval(cropped_cloud, 0.001, 50000, tmp_cloud);
	cropped_cloud->points.clear();
	*cropped_cloud += *tmp_cloud;

	Eigen::Vector4d centroid_3d;
	pcl::compute3DCentroid(*cropped_cloud, centroid_3d);

	//plane fitting and outlier removal
#if 0	
	//shrink borders of the square surface
	PointCloudT::Ptr shrinked_marker(new PointCloudT);
	for (auto p : cropped_cloud->points)
	{
		float dist = abs(p.x-centroid_3d(0));
		float tmp = abs(p.y - centroid_3d(1));
		if (tmp > dist) dist = tmp;
		tmp = abs(p.z - centroid_3d(2));
		if (tmp > dist) dist = tmp;

		if (dist < 0.04f)
		{
			p.b = 128;
			shrinked_marker->push_back(p);
		}
	}

	viewer_->addPointCloud(shrinked_marker, "shrinked_marker", 0);
	//viewer_->spin();

	if (shrinked_marker->points.size() > 100)
	{
		PointCloudT::Ptr filtered_marker(new PointCloudT);

		pcl::PCA<PointT> pca(*shrinked_marker);

		Eigen::Vector3f nz = pca.getEigenVectors().real().col(2);

		for (auto p : cropped_cloud->points)
		{
			Eigen::Vector3f vec(p.x- centroid_3d(0), p.y - centroid_3d(1), p.z - centroid_3d(2));
			float angle = acos(nz.dot(vec)/vec.norm())/M_PI*180.;

			if (abs(angle-90.f) < .5f)
			{
				filtered_marker->push_back(p);
			}
			
		}

		cropped_cloud->points.clear();
		*cropped_cloud += *filtered_marker;

		//viewer_->addPointCloud(filtered_marker, "filtered_marker", 0);
		//viewer_->spin();
	}
#endif

	pcl::ModelCoefficients sphere; sphere.values.resize(4);
	sphere.values[0] = centroid_3d(0); sphere.values[1] = centroid_3d(1); sphere.values[2] = centroid_3d(2);
	sphere.values[3] = 0.002;

	viewer_->addSphere(sphere, "sphere", 0);

	tmp_cloud->points.clear();

	for (int i = 0; i < cropped_cloud->points.size(); i++)
	{
		double dist = abs(cropped_cloud->points[i].x - centroid_3d(0));
		double tmp = abs(cropped_cloud->points[i].y - centroid_3d(1));
		if (tmp > dist) dist = tmp;
		tmp = abs(cropped_cloud->points[i].z - centroid_3d(2));
		if (tmp > dist) dist = tmp;

		if (dist < 0.04)
			tmp_cloud->push_back(cropped_cloud->points[i]);
	}

	Eigen::Matrix4f base_to_marker_guess = Eigen::Matrix4f::Identity();
	base_to_marker_guess.col(3).head(3) = centroid_3d.head(3).cast<float>();

	pcl::transformPointCloud(*marker_template, *marker_template, base_to_marker_guess);

	pcl::IterativeClosestPoint<PointT, PointT> icp;
	icp.setInputSource(marker_template);
	icp.setInputTarget(cropped_cloud);
	icp.setTransformationEpsilon(1e-15);
	icp.setMaxCorrespondenceDistance(0.05);
	icp.setMaximumIterations(200);
	icp.setEuclideanFitnessEpsilon(1e-15);
	PointCloudT::Ptr marker_final(new PointCloudT);
	icp.align(*marker_final);
	

	std::cout << "fitness score: " <<icp.getFitnessScore() << std::endl;

	Eigen::Matrix4d marker_to_gripper = Eigen::Matrix4d::Identity();

	marker_to_gripper.topLeftCorner<3, 3>() = Eigen::AngleAxisd(M_PI, Eigen::Vector3d::UnitX()).matrix()*Eigen::AngleAxisd(-M_PI*0.25, Eigen::Vector3d::UnitZ()).matrix();

	// icp transform is not frame transform, is source cloud to target cloud transform in base
	Eigen::Matrix4d base_to_marker = icp.getFinalTransformation().cast<double>()*base_to_marker_guess.cast<double>();

	std::cout << "base to marker:\n" << base_to_marker << "\n";

	pcl::transformPointCloud(*marker_template_copy, *marker_final, base_to_marker.cast<float>());

	PointCloudT::Ptr pot_centers_in_base(new PointCloudT);

	pcl::transformPointCloud(*pot_centers, *pot_centers_in_base, base_to_marker.cast<float>());

	for (int i = 0; i < pot_centers_in_base->points.size(); i++)
	{
		pcl::ModelCoefficients sphere; sphere.values.resize(4);
		sphere.values[0] = pot_centers_in_base->points[i].x; sphere.values[1] = pot_centers_in_base->points[i].y; sphere.values[2] = pot_centers_in_base->points[i].z;
		sphere.values[3] = 0.001;
		viewer_->addSphere(sphere, "sphere"+std::to_string(i), 0);
	}

	cv::Mat cloud_cv;
	const int cloud_size = cropped_cloud->points.size();
	cloud_cv.create(1, cloud_size, CV_32F);
	for (int i = 0; i < cloud_size; i++)
		cloud_cv.ptr<float>(0)[i] = cropped_cloud->points[i].z;

	cv::Mat cloud_cv_n;
	cv::normalize(cloud_cv, cloud_cv_n, 0, 255, cv::NORM_MINMAX, CV_8UC1);

	cv::Mat colormap;
	cv::applyColorMap(cloud_cv_n, colormap, cv::COLORMAP_JET);

	for (int i=0; i<cloud_size; i++)
	{
		cv::Vec3b bgr = colormap.at<cv::Vec3b>(i);
		cropped_cloud->points[i].b = bgr.val[0];
		cropped_cloud->points[i].g = bgr.val[1];
		cropped_cloud->points[i].r = bgr.val[2];
	}

	viewer_->addPointCloud(marker_final, "marker_final", 0);
	//viewer_->addPointCloud(marker_template, "3d_marker");
	viewer_->addPointCloud(cropped_cloud, "cropped_cloud");
	viewer_->spin();

	//return;

	if (!icp.hasConverged())
	{
		std::cout << "icp not converged\n";
		return;
	}

	//return;
	robot_arm_client_->moveHandRelativeTranslate(0, 0, 0.1, 0.1, 0.2);
	robot_arm_client_->rotateJointRelative(4, 180., 0.6, 0.6);
	double lift_height = 0.4;
	
	//for (int y = -2; y <= 2; y++)
	for (int y = -1; y <= -1; y++)
	//for (int y = 0; y <= 0; y++)
	{
		//for (int x = 3; x >= -3; x--)
		for (int x = 2; x >= 1; x--)
		//for (int x = 0; x <= 0; x++)
		{
			if (y == 0 && x == 0) continue;

			std::getchar();

			marker_to_gripper(0, 3) = (double)x*marker_length_/*+ marker_width*0.5*/;
			marker_to_gripper(1, 3) = (double)y*marker_length_/*+ marker_width*0.5*/;
			marker_to_gripper(2, 3) = 0.0;

			//Eigen::Matrix4d probe_pose_eigen = base_to_marker*marker_to_gripper*probe_to_hand_;
			Eigen::Matrix4d probe_pose_eigen = base_to_marker*marker_to_gripper*hand_to_gripper_;
			std::cout << "probe pose:\n" << probe_pose_eigen << "\n";
			// move arm
			double pose[6];
			eigenMat4dToArray6(probe_pose_eigen, pose);
			//continue;
			// probe
			/*{
				robot_arm_client_->moveHandL(pose, 0.1, 0.1);
				robot_arm_client_->waitTillHandReachDstPose(pose);
				continue;
			}*/

			//on top of the pot
			pose[2] += lift_height;
			robot_arm_client_->moveHandL(pose, 0.1, 0.2);
			robot_arm_client_->waitTillHandReachDstPose(pose);

			//go down
			pose[2] -= lift_height-0.1;
			robot_arm_client_->moveHandL(pose, 0.1, 0.1);
			robot_arm_client_->waitTillHandReachDstPose(pose);

			pose[2] -= 0.1;
			robot_arm_client_->moveHandL(pose, 0.05, 0.05);
			robot_arm_client_->waitTillHandReachDstPose(pose);

			gripper_.close();

			//std::getchar();
			Sleep(500);

			//go up
			pose[2] += lift_height;
			robot_arm_client_->moveHandL(pose, 0.1, 0.5);
			robot_arm_client_->waitTillHandReachDstPose(pose);

			//continue;

			//top of the balance
			pose[0] = -0.144; pose[1] = 0.375; pose[2] = 0.5, pose[3] = 0; pose[4] = 0; pose[5] = -1.047;
			robot_arm_client_->moveHandL(pose, 0.2, 0.2);
			robot_arm_client_->waitTillHandReachDstPose(pose);
			

			//move down
			pose[0] = -0.144; pose[1] = 0.375; pose[2] = 0.101, pose[3] = 0; pose[4] = 0; pose[5] = -1.047;
			robot_arm_client_->moveHandL(pose, 0.2, 0.2);
			robot_arm_client_->waitTillHandReachDstPose(pose);

			gripper_.open();

			//std::getchar();
			Sleep(2000);

			gripper_.close();

			//go up
			pose[2] = 0.667;
			robot_arm_client_->moveHandL(pose, 0.3, 0.3);
			robot_arm_client_->waitTillHandReachDstPose(pose);
		

			// 0.248, -0.317, 0.667, 0.055,-0.097,-3.8904
			// base 266.57 deg
			//-0.144, 0.375, 0.101, 0, 0, -1.047	//place pot on balance
			// z = 0.667	
			// base 103.88

			eigenMat4dToArray6(probe_pose_eigen, pose);

			//on top of the pot
			pose[2] += lift_height;
			robot_arm_client_->moveHandL(pose, 0.5, 0.5);
			robot_arm_client_->waitTillHandReachDstPose(pose);

			//go down fast
			pose[2] -= lift_height - 0.1;
			robot_arm_client_->moveHandL(pose, 0.2, 0.2);
			robot_arm_client_->waitTillHandReachDstPose(pose);

			//go down slow
			pose[2] -= 0.1;
			robot_arm_client_->moveHandL(pose, 0.05, 0.05);
			robot_arm_client_->waitTillHandReachDstPose(pose);

			gripper_.open();

			//go up
			pose[2] += lift_height;
			robot_arm_client_->moveHandL(pose, 0.2, 0.2);
			robot_arm_client_->waitTillHandReachDstPose(pose);
		}
	}
}

bool VisionArmCombo::localizeByScanSpheres(int robot_stop, int side)
{
	if (robot_arm_client_ == NULL) initRobotArmClient();
	if (line_profiler_ == NULL) initLineProfiler();
	
	Eigen::Matrix4f cur_pose; getCurHandPose(cur_pose);

	if (cur_pose(2, 2) >= 0.)	//check z direction
	{
		std::cout << "gripper pointing down!\n";
		return false;
	}

	double sign;
	if (side == 0) sign = 1.0;
	else if (side == 1) sign = -1.0;
	else std:cout << "wrong side\n";

	double array6[6];

	// if hand is on the other side, go to an intermediate position first
	if (cur_pose(0, 3)*sign < 0.)	
	{
		double cur_joints[6];
		robot_arm_client_->getCurJointPose(cur_joints);

		array6[0] = 90; array6[1] = -90;
		array6[2] = -90; array6[3] = -90;
		array6[4] = 90; array6[5] = 0;
		for (int i = 0; i < 6; i++)	array6[i] = array6[i] / 180.*M_PI;

		double dist = 0.;
		for (int i = 0; i < 6; i++)
		{
			dist += std::abs(cur_joints[i] - array6[i]);
		}

		if (dist > 0.01)
		{
			//check hand x coordinate
			if (cur_pose(0, 3) > 0)	//left side table 0
			{
				array6[0] = 0; array6[1] = -90;
				array6[2] = -90; array6[3] = -90;
				array6[4] = 90; array6[5] = 0;
				for (int i = 0; i < 6; i++)	array6[i] = array6[i] / 180.*M_PI;
				robot_arm_client_->moveHandJ(array6, move_joint_speed_, move_joint_acceleration_, true);

				std::cout << "above table 0\n";
			}
			else
			{
				array6[0] = 180; array6[1] = -90;
				array6[2] = -90; array6[3] = -90;
				array6[4] = 90; array6[5] = 0;
				for (int i = 0; i < 6; i++)	array6[i] = array6[i] / 180.*M_PI;
				robot_arm_client_->moveHandJ(array6, move_joint_speed_, move_joint_acceleration_, true);

				std::cout << "above table 1\n";
			}

			//above balance
			array6[0] = 90; array6[1] = -90;
			array6[2] = -90; array6[3] = -90;
			array6[4] = 90; array6[5] = 0;
			for (int i = 0; i < 6; i++)	array6[i] = array6[i] / 180.*M_PI;
			robot_arm_client_->moveHandJ(array6, move_joint_speed_, move_joint_acceleration_, true);

			std::cout << "above balance\n";
		}
	}

	if (side == 0)
	{
		array6[0] = 0; array6[1] = -90;
		array6[2] = -90; array6[3] = -90;
		array6[4] = 90; array6[5] = 0;
		for (int i = 0; i < 6; i++)	array6[i] = array6[i] / 180.*M_PI;
		robot_arm_client_->moveHandJ(array6, move_joint_speed_, move_joint_acceleration_, true);

		std::cout << "above table 0\n";
	}
	else if (side == 1)
	{
		array6[0] = 180; array6[1] = -90;
		array6[2] = -90; array6[3] = -90;
		array6[4] = 90; array6[5] = 0;
		for (int i = 0; i < 6; i++)	array6[i] = array6[i] / 180.*M_PI;
		robot_arm_client_->moveHandJ(array6, move_joint_speed_, move_joint_acceleration_, true);

		std::cout << "above table 1\n";
	}

	// first ball
	Eigen::Vector3d hand_pos;
	hand_pos << sign*0.44, -0.385, 0.44;

	scanSphereOnTable(hand_pos, sphere_pos1_, side);

	if (sphere_pos1_(2) < 0.04 || sphere_pos1_(2) > 0.16)
	{
		std::cout << "sphere position out of range\n";
		return false;
	}

	// second ball the other side
	hand_pos(0) += sign*0.685; hand_pos(1) += 0.305;
	scanSphereOnTable(hand_pos, sphere_pos2_, side);

	if (sphere_pos2_(2) < 0.04 || sphere_pos2_(2) > 0.16)
	{
		std::cout << "sphere position out of range\n";
		return false;
	}

	// third ball
	hand_pos(0) -= sign*0.685; hand_pos(1) += 0.3048;

	scanSphereOnTable(hand_pos, sphere_pos3_, side);

	if (sphere_pos3_(2) < 0.04 || sphere_pos3_(2) > 0.16)
	{
		std::cout << "sphere position out of range\n";
		return false;
	}

//	std::cout << "1-3: " << (sphere_pos1_ - sphere_pos3_).norm() << "\n";
//	std::cout << "1-2: " << (sphere_pos1_ - sphere_pos2_).norm() << "\n";
//	std::cout << "3-2: " << (sphere_pos3_ - sphere_pos2_).norm() << "\n";

	//table surface normal
	table_normal_y_ = (sphere_pos3_ - sphere_pos1_).normalized();
	// projection of sphere 2 on the line of sphere 1 and sphere 3
	Eigen::Vector3d p = sphere_pos1_ + (sphere_pos2_ - sphere_pos1_).dot(table_normal_y_)*table_normal_y_;
	if(side == 0) // left
		table_normal_x_ = (sphere_pos2_ - p).normalized();	
	else
		table_normal_x_ = (p - sphere_pos2_).normalized();

	table_normal_z_ = table_normal_x_.cross(table_normal_y_);

	hand_pose_above_pot_eigen_ = Eigen::Matrix4d::Identity();
	hand_pose_above_pot_eigen_.col(0).head<3>() = -sign*table_normal_y_;
	hand_pose_above_pot_eigen_.col(1).head<3>() = -sign*table_normal_x_;
	hand_pose_above_pot_eigen_.col(2).head<3>() = -table_normal_z_;

	display();

	return true;
}

void VisionArmCombo::gotoPot(int robot_stop, int side, int local_pot_x, int local_pot_y, bool open_gripper)
{
	Eigen::Matrix4f cur_pose; getCurHandPose(cur_pose);

	if (cur_pose(2, 2) <= 0.)	//check z direction
	{
		std::cout << "gripper pointing up!\n";
		return;
	}

	double sign;
	if (side == 0) sign = 1.0;
	else if (side == 1) sign = -1.0;
	else std::cout << "wrong side in goto pot\n";
	
	hand_pose_above_pot_eigen_.col(3).head<3>() = sphere_pos3_
		+ table_normal_z_*(0.15)
		+ sign*(table_offset_x_ + local_pot_x*pot2pot_dist_)*table_normal_x_
		+ (table_offset_y_ - local_pot_y*pot2pot_dist_)*table_normal_y_
		;

	// move to pot 
	Eigen::Matrix4d tmp = hand_pose_above_pot_eigen_*hand_to_gripper_.inverse();
	//Eigen::Matrix4d tmp = hand_pose_above_pot_eigen_;
	double array6[6];
	eigenMat4dToArray6(tmp, array6);
	robot_arm_client_->moveHandL(array6, move_arm_acceleration_, move_arm_speed_);
	robot_arm_client_->waitTillHandReachDstPose(array6);

	//return;

	hand_pose_above_pot_eigen_.col(3).head<3>() += table_normal_z_*(-0.20);

	tmp = hand_pose_above_pot_eigen_*hand_to_gripper_.inverse();
	eigenMat4dToArray6(tmp, array6);
	robot_arm_client_->moveHandL(array6, move_arm_acceleration_, move_arm_speed_);
	robot_arm_client_->waitTillHandReachDstPose(array6);

	if (open_gripper) gripper_.open();
	else gripper_.close();

	hand_pose_above_pot_eigen_.col(3).head<3>() += table_normal_z_*(0.20);

	tmp = hand_pose_above_pot_eigen_*hand_to_gripper_.inverse();
	eigenMat4dToArray6(tmp, array6);
	robot_arm_client_->moveHandL(array6, move_arm_acceleration_, move_arm_speed_);
	robot_arm_client_->waitTillHandReachDstPose(array6);

	std::cout << "reached\n";
}

void VisionArmCombo::scanPot(int robot_stop, int side, int local_pot_x, int local_pot_y)
{
	Eigen::Matrix4f cur_pose; getCurHandPose(cur_pose);

	if (cur_pose(2, 2) >= 0.)	//check z direction
	{
		std::cout << "gripper pointing down!\n";
		return;
	}

	double sign;
	if (side == 0) sign = 1.0;
	else if (side == 1) sign = -1.0;
	else std:cout << "wrong side in scan pot\n";
	
	hand_pose_above_pot_eigen_.col(3).head<3>() = sphere_pos3_
		+ table_normal_z_*(0.17)
		+ sign*(table_offset_x_ + local_pot_x*pot2pot_dist_)*table_normal_x_
		+ (table_offset_y_ - local_pot_y*pot2pot_dist_)*table_normal_y_
		;

	// move to pot 
	Eigen::Matrix4d hand_to_scan_start = Eigen::Matrix4d::Identity();
	Eigen::Matrix4d hand_to_scan_end = Eigen::Matrix4d::Identity();

	hand_to_scan_start(0, 3) = -0.06;
	hand_to_scan_end(0, 3) = 0.06;

	Eigen::Matrix4d start_scan_hand_pose = hand_pose_above_pot_eigen_*hand_to_scan_start;
	Eigen::Matrix4d end_scan_hand_pose = hand_pose_above_pot_eigen_*hand_to_scan_end;

	start_scan_hand_pose.block<3,3>(0,0) *= Eigen::AngleAxisd(-0.5*M_PI, Eigen::Vector3d::UnitZ()).matrix();

	end_scan_hand_pose.block<3, 3>(0, 0) *= Eigen::AngleAxisd(-0.5*M_PI, Eigen::Vector3d::UnitZ()).matrix();

	start_scan_hand_pose *= handToScanner_.cast<double>().inverse();
	end_scan_hand_pose *= handToScanner_.cast<double>().inverse();

	double array6[6];
	eigenMat4dToArray6(start_scan_hand_pose, array6);
	robot_arm_client_->moveHandL(array6, 0.1, 0.1);
	robot_arm_client_->waitTillHandReachDstPose(array6);

	Sleep(1000);

	Eigen::Vector3d translation = end_scan_hand_pose.col(3).head<3>() - start_scan_hand_pose.col(3).head<3>();

	double tran[3] = { translation(0), translation(1), translation(2) };
	
	PointCloudT::Ptr scan_cloud(new PointCloudT);

	getCurHandPose(cur_pose);

	line_profiler_->m_vecProfileData.clear();
	scanTranslateOnly(tran, scan_cloud, scan_acceleration_, scan_speed_);

	pcl::transformPointCloud(*scan_cloud, *scan_cloud, cur_pose*handToScanner_);

	hand_pose_above_pot_eigen_.col(3).head<3>() -= table_normal_z_*(0.07);
	Eigen::Matrix4d hand_pose_for_rgb = hand_pose_above_pot_eigen_*hand_to_rgb_.inverse();

	eigenMat4dToArray6(hand_pose_for_rgb, array6);
	robot_arm_client_->moveHandL(array6, 0.1, 0.1);
	robot_arm_client_->waitTillHandReachDstPose(array6);
	
	Sleep(1000);
	exo_rgb_cam_->acquireRGBImage();
	cv::Mat undistort;
	cv::undistort(exo_rgb_cam_->rgb_frame, undistort, kinect_rgb_camera_matrix_cv_, kinect_rgb_dist_coeffs_cv_);
	cv::Mat tmp;
	cv::resize(undistort, tmp, cv::Size(), 0.5, 0.5);
	cv::imshow("exo", tmp);
	cv::waitKey(1000);

	std::vector<cv::Point3f> object_points(scan_cloud->points.size());
	for (int i=0; i<scan_cloud->points.size(); i++)
	{
		object_points[i].x = scan_cloud->points[i].x;
		object_points[i].y = scan_cloud->points[i].y;
		object_points[i].z = scan_cloud->points[i].z;
	}
	
	Eigen::Matrix4d tmp_inverse = hand_pose_above_pot_eigen_.inverse();

	cv::Mat rot, tvec, rvec;
	rot.create(3, 3, CV_64F);
	tvec.create(3, 1, CV_64F);
	for (int y = 0; y < 3; y++)
		for (int x = 0; x < 3; x++)
			rot.at<double>(y, x) = tmp_inverse(y, x);

	tvec.at<double>(0, 0) = tmp_inverse(0, 3);
	tvec.at<double>(1, 0) = tmp_inverse(1, 3);
	tvec.at<double>(2, 0) = tmp_inverse(2, 3);

	cv::Rodrigues(rot, rvec);
	std::vector<cv::Point2f> img_points;
	cv::projectPoints(object_points, rvec, tvec, kinect_rgb_camera_matrix_cv_, kinect_rgb_dist_coeffs_cv_, img_points);

	for (int i = 0; i < scan_cloud->points.size(); i++)
	{
		int x = std::round(img_points[i].x);
		int y = std::round(img_points[i].y);

		//std::cout << x << "  " << y << "\n";

		if (x >= 0 && x < 1920 && y >= 0 && y < 1200)
		{
			scan_cloud->points[i].b = exo_rgb_cam_->rgb_frame.at<cv::Vec3b>(y, x).val[0];
			scan_cloud->points[i].g = exo_rgb_cam_->rgb_frame.at<cv::Vec3b>(y, x).val[1];
			scan_cloud->points[i].r = exo_rgb_cam_->rgb_frame.at<cv::Vec3b>(y, x).val[2];
		}
	}

	viewer_->addPointCloud(scan_cloud, "pot_" + std::to_string(local_pot_x)+"_"+ std::to_string(local_pot_y), 0);
	viewer_->spin();
}

void VisionArmCombo::registerRGBandPointCloud(cv::Mat & rgb, PointCloudT::Ptr cloud_in_base, Eigen::Matrix4d & rgb_pose, bool clear_viewer)
{
	std::vector<cv::Point3f> object_points(cloud_in_base->points.size());
	for (int i = 0; i<cloud_in_base->points.size(); i++)
	{
		object_points[i].x = cloud_in_base->points[i].x;
		object_points[i].y = cloud_in_base->points[i].y;
		object_points[i].z = cloud_in_base->points[i].z;
	}

	Eigen::Matrix4d tmp_inverse = rgb_pose.inverse();

	cv::Mat rot, tvec, rvec;
	rot.create(3, 3, CV_64F);
	tvec.create(3, 1, CV_64F);
	for (int y = 0; y < 3; y++)
		for (int x = 0; x < 3; x++)
			rot.at<double>(y, x) = tmp_inverse(y, x);

	tvec.at<double>(0, 0) = tmp_inverse(0, 3);
	tvec.at<double>(1, 0) = tmp_inverse(1, 3);
	tvec.at<double>(2, 0) = tmp_inverse(2, 3);

	cv::Rodrigues(rot, rvec);
	std::vector<cv::Point2f> img_points;
	cv::projectPoints(object_points, rvec, tvec, kinect_rgb_camera_matrix_cv_, kinect_rgb_dist_coeffs_cv_, img_points);

	for (int i = 0; i < cloud_in_base->points.size(); i++)
	{
		int x = std::round(img_points[i].x);
		int y = std::round(img_points[i].y);

		//std::cout << x << "  " << y << "\n";

		if (x >= 0 && x < 1920 && y >= 0 && y < 1200)
		{
			cloud_in_base->points[i].b = exo_rgb_cam_->rgb_frame.at<cv::Vec3b>(y, x).val[0];
			cloud_in_base->points[i].g = exo_rgb_cam_->rgb_frame.at<cv::Vec3b>(y, x).val[1];
			cloud_in_base->points[i].r = exo_rgb_cam_->rgb_frame.at<cv::Vec3b>(y, x).val[2];
		}
	}

#if 0
	pcl::NormalEstimation<PointT, pcl::Normal> ne;
	ne.setInputCloud(cloud_in_base);
	ne.setRadiusSearch(0.002);

	pcl::PointCloud<pcl::Normal>::Ptr cloud_normals(new pcl::PointCloud<pcl::Normal>());
	ne.compute(*cloud_normals);

	std::cout << "normal done\n";

	pcl::PointCloud<pcl::PointNormal>::Ptr cloud_smoothed_normals(new pcl::PointCloud<pcl::PointNormal>());

	pcl::PointCloud<pcl::PointXYZ>::Ptr cloud(new pcl::PointCloud<pcl::PointXYZ>);

	pcl::copyPointCloud(*cloud_in_base, *cloud);

	pcl::concatenateFields(*cloud, *cloud_normals, *cloud_smoothed_normals);

	pcl::Poisson<pcl::PointNormal> poisson;
	poisson.setDepth(9);
	poisson.setInputCloud(cloud_smoothed_normals);
	pcl::PolygonMesh mesh;
	poisson.reconstruct(mesh);

	viewer_->addPolygonMesh(mesh, "mesh", 0);
	viewer_->spin();
#endif
	if(clear_viewer)
		viewer_->removeAllPointClouds();
	std::string name = "rgb_cloud" + std::to_string(cv::getTickCount());
	viewer_->addPointCloud(cloud_in_base, name);
	viewer_->setPointCloudRenderingProperties(pcl::visualization::PCL_VISUALIZER_POINT_SIZE, 2, name);
	display();
	if(clear_viewer)
		viewer_->removeAllPointClouds();
}

void VisionArmCombo::transportPotOnBalance(bool open_finger_on_balance)
{
	double array6[6];

	//move above balance
	array6[0] = 124.05;	array6[1] = -49.11;
	array6[2] = -129.18; array6[3] = -91.61;
	array6[4] = 270.06; array6[5] = -34.01;
	for (int i = 0; i < 6; i++)	array6[i] = array6[i] / 180.*M_PI;

#ifndef JUST_SCAN_A_POT
	robot_arm_client_->moveHandJ(array6, move_joint_speed_, move_joint_acceleration_, true);
#endif

	std::cout << "move above balance done\n";

	// put it down
	array6[0] = -0.020; array6[1] = 0.3458; array6[2] = 0.500;
	array6[3] = 0; array6[4] = 0; array6[5] = M_PI;
#ifndef JUST_SCAN_A_POT
	robot_arm_client_->moveHandL(array6, 0.1, 0.1);
	robot_arm_client_->waitTillHandReachDstPose(array6);
#endif

	std::cout << "put it down\n";

	// final
	array6[0] = -0.02; array6[1] = 0.3458; array6[2] = 0.351;
	array6[3] = 0; array6[4] = 0; array6[5] = M_PI;
#ifndef JUST_SCAN_A_POT
	robot_arm_client_->moveHandL(array6, 0.1, 0.05);
	robot_arm_client_->waitTillHandReachDstPose(array6);
#endif

	array6ToEigenMat4d(array6, release_pose_);

#ifndef JUST_SCAN_A_POT

	if (open_finger_on_balance)
		gripper_.open();
	else
		gripper_.close();

	Sleep(500);

	// bring it up
	array6[0] = -0.020; array6[1] = 0.3458; array6[2] = 0.500;
	array6[3] = 0; array6[4] = 0; array6[5] = M_PI;
	robot_arm_client_->moveHandL(array6, 0.1, 0.1);
	robot_arm_client_->waitTillHandReachDstPose(array6);

	std::cout << "bring it up\n";

	//move above balance
	array6[0] = 124.05;	array6[1] = -49.11;
	array6[2] = -129.18; array6[3] = -91.61;
	array6[4] = 270.06; array6[5] = -34.01;
	for (int i = 0; i < 6; i++)	array6[i] = array6[i] / 180.*M_PI;
	robot_arm_client_->moveHandJ(array6, move_joint_speed_, move_joint_acceleration_, true);

	std::cout << "move above balance\n";
#endif
}

void VisionArmCombo::gotoBalance(int pot_id)
{
	double array6[6];

	transportPotOnBalance(true);

#ifndef JUST_SCAN_A_POT
	//switch camera
	robot_arm_client_->getCurJointPose(array6);
	array6[4] = M_PI_2;
	robot_arm_client_->moveHandJ(array6, move_joint_speed_, move_joint_acceleration_, true);
#endif

	std::vector<PointCloudT::Ptr> cloud_vec;

	std::vector<std::vector<double>> scan_start_poses;

	std::vector<std::vector<double>> scan_translations;
	
	std::vector<double> scan_start_pose_0 = { -0.2, 0.439, 0.35, M_PI, 0, 0 };
	scan_start_poses.push_back(scan_start_pose_0);
	std::vector<double> scan_translation_0 = { 0.15, 0, 0};
	scan_translations.push_back(scan_translation_0);

	std::vector<double> scan_start_pose_1 = { 0.093, 0.439, 0.298, 3.0414, 0, -0.787};
	scan_start_poses.push_back(scan_start_pose_1);
	std::vector<double> scan_translation_1 = { -0.15, 0, 0 };
	scan_translations.push_back(scan_translation_1);

	std::vector<double> scan_start_pose_2 = { -0.38, 0.439, 0.298, 3.0414, 0, 0.787 };
	scan_start_poses.push_back(scan_start_pose_2);
	std::vector<double> scan_translation_2 = { 0.15, 0, 0 };
	scan_translations.push_back(scan_translation_2);

	std::vector<double> scan_start_pose_3 = { -0.067, 0.612, 0.344, 1.8483, 1.9444, -0.5613 };
	scan_start_poses.push_back(scan_start_pose_3);
	std::vector<double> scan_translation_3 = { 0, -0.15, 0 };
	scan_translations.push_back(scan_translation_3);

	std::vector<double> scan_start_pose_4 = { -0.067, 0.287, 0.35, 2.1692, 2.2741, 0.0288 };
	scan_start_poses.push_back(scan_start_pose_4);
	std::vector<double> scan_translation_4 = { 0, 0.15, 0 };
	scan_translations.push_back(scan_translation_4);

#if 1
	Eigen::Matrix4d rgb_pose = release_pose_*hand_to_gripper_;
	rgb_pose.block<3, 3>(0, 0) = Eigen::Matrix3d::Identity();
	rgb_pose(1, 1) = -1.;
	rgb_pose(2, 2) = -1.;
	rgb_pose(2, 3) = 0.17;

	Eigen::Matrix4d hand_pose = rgb_pose*hand_to_rgb_.inverse();
	eigenMat4dToArray6(hand_pose, array6);

	robot_arm_client_->moveHandL(array6, move_arm_acceleration_, move_arm_speed_);
	robot_arm_client_->waitTillHandReachDstPose(array6);

	Sleep(1000);
	exo_rgb_cam_->acquireRGBImage();
	cv::Mat undistort;
	cv::undistort(exo_rgb_cam_->rgb_frame, undistort, kinect_rgb_camera_matrix_cv_, kinect_rgb_dist_coeffs_cv_);
	cv::Mat tmp;
	cv::resize(undistort, tmp, cv::Size(), 0.5, 0.5);
	cv::imshow("exo", tmp);
	cv::waitKey(100);
#endif

	std::string name="";

	

	if (pot_id < pot_labels.size()) {

		name = "Data\\" + pot_labels[pot_id];
		cv::imwrite(name + ".bmp", undistort);
		cv::FileStorage fs(name + ".yml", cv::FileStorage::WRITE);

		cv::Mat rgb_pose_cv;
		
		EigenMatrix4dToCVMat4d(rgb_pose, rgb_pose_cv);

		fs << "rgb_pose" << rgb_pose_cv;

		fs.release();
	}

	for (int i = 0; i < scan_start_poses.size(); i++) {

		robot_arm_client_->moveHandL(scan_start_poses[i].data(), move_arm_acceleration_, move_arm_speed_);
		robot_arm_client_->waitTillHandReachDstPose(scan_start_poses[i].data());
		Sleep(1000);

		PointCloudT::Ptr scan_cloud(new PointCloudT);
		PointCloudT::Ptr tmp_cloud(new PointCloudT);

		Eigen::Matrix4f cur_pose; getCurHandPose(cur_pose);
		
		line_profiler_->m_vecProfileData.clear();
		scanTranslateOnly(scan_translations[i].data(), scan_cloud, scan_acceleration_, scan_speed_);

		pcl::transformPointCloud(*scan_cloud, *tmp_cloud, cur_pose*handToScanner_);

		pass_.setInputCloud(tmp_cloud);
		pass_.setNegative(false);
		pass_.setFilterFieldName("z");
		pass_.setFilterLimits(0.01, 0.1);
		pass_.filter(*scan_cloud);

		// empty point cloud, return;
		if (scan_cloud->size() < 100)
			return;

		uint32_t rgb;

		if (i == 0) rgb = ((uint32_t)255 << 16);
		else if (i == 1) rgb = ((uint32_t)255 << 8);
		else if (i == 2) rgb = ((uint32_t)255);
		else if (i == 3) rgb = ((uint32_t)255<<16 | (uint32_t)255 << 8);

		for (auto & p : scan_cloud->points) 
			p.rgb = *reinterpret_cast<float*>(&rgb);

		cloud_vec.push_back(scan_cloud);

		//viewer_->addPointCloud(scan_cloud, "scan_cloud"+std::to_string(i), 0);
		//viewer_->spin();
		//viewer_->removePointCloud("scan_cloud"+std::to_string(i));
#if 0
		Eigen::Matrix4d rgb_pose = cur_pose.cast<double>();
		rgb_pose.col(3).head<3>() = (cur_pose.cast<double>() * handToScanner_.col(3).cast<double>()).head<3>();

		rgb_pose.col(3).head<3>() += rgb_pose.col(2).head<3>()*0.05;

		rgb_pose(0, 3) += 0.5*scan_translations[i][0];
		rgb_pose(1, 3) += 0.5*scan_translations[i][1];
		rgb_pose(2, 3) += 0.5*scan_translations[i][2];

		std::cout << "rgb_pose:\n" << rgb_pose << "\n";

		Eigen::Vector3d z_axis = rgb_pose.col(2).head<3>();

	//	rgb_pose.block<3, 3>(0, 0) = rgb_pose.block<3, 3>(0, 0)*Eigen::AngleAxisd(-0.5*M_PI, z_axis).matrix();

		//std::cout << "rgb_pose:\n" << rgb_pose << "\n";


		Eigen::Matrix4d hand_pose = rgb_pose*hand_to_rgb_.inverse();
		eigenMat4dToArray6(hand_pose, array6);

		std::cout << "hand_pose:\n" << hand_pose << "\n";

		std::getchar();

		robot_arm_client_->moveHandL(array6, move_arm_acceleration_, move_arm_speed_);
		robot_arm_client_->waitTillHandReachDstPose(array6);

		Sleep(1000);
		exo_rgb_cam_->acquireRGBImage();
		cv::Mat undistort;
		cv::undistort(exo_rgb_cam_->rgb_frame, undistort, kinect_rgb_camera_matrix_cv_, kinect_rgb_dist_coeffs_cv_);
		cv::Mat tmp;
		cv::resize(undistort, tmp, cv::Size(), 0.5, 0.5);
		cv::imshow("exo", tmp);
		cv::waitKey(100);
#endif
		registerRGBandPointCloud(exo_rgb_cam_->rgb_frame, scan_cloud, rgb_pose, true);

#if 1
		if (pot_id < pot_labels.size())
		{
			pcl::io::savePCDFileBinary(name +"_scan_"+std::to_string(i)+ ".pcd", *scan_cloud);
		}
#endif
	}

	//fs.release();
	getchar();
	return;
	
#if 0
	// move to scan start pose
	array6[0] = -0.2; array6[1] = 0.439; array6[2] = 0.35;
	array6[3] = M_PI; array6[4] = 0; array6[5] = 0;
	robot_arm_client_->moveHandL(array6, move_arm_acceleration_, move_arm_speed_);
	robot_arm_client_->waitTillHandReachDstPose(array6);
	Sleep(1000);

	PointCloudT::Ptr scan_cloud(new PointCloudT);
	PointCloudT::Ptr tmp_cloud(new PointCloudT);

	Eigen::Matrix4f cur_pose; getCurHandPose(cur_pose);

	double tran[3] = {0.15, 0, 0};
	line_profiler_->m_vecProfileData.clear();
	scanTranslateOnly(tran, scan_cloud, scan_acceleration_, scan_speed_);

	pcl::transformPointCloud(*scan_cloud, *tmp_cloud, cur_pose*handToScanner_);

	pass_.setInputCloud(tmp_cloud);
	pass_.setNegative(false);
	pass_.setFilterFieldName("z");
	pass_.setFilterLimits(-0.04, 0.1);
	pass_.filter(*scan_cloud);

//	viewer_->addPointCloud(scan_cloud, "scan_cloud", 0);
//	viewer_->spin();
//	viewer_->removePointCloud("scan_cloud", 0);

	Eigen::Matrix4d rgb_pose = release_pose_*hand_to_gripper_;
	rgb_pose.block<3, 3>(0, 0) = Eigen::Matrix3d::Identity();
	rgb_pose(1, 1) = -1.;
	rgb_pose(2, 2) = -1.;
	rgb_pose(2, 3) = 0.17;

	Eigen::Matrix4d hand_pose = rgb_pose*hand_to_rgb_.inverse();
	eigenMat4dToArray6(hand_pose, array6);

	robot_arm_client_->moveHandL(array6, move_arm_acceleration_, move_arm_speed_);
	robot_arm_client_->waitTillHandReachDstPose(array6);

	Sleep(1000);
	exo_rgb_cam_->acquireRGBImage();
	cv::Mat undistort;
	cv::undistort(exo_rgb_cam_->rgb_frame, undistort, kinect_rgb_camera_matrix_cv_, kinect_rgb_dist_coeffs_cv_);
	cv::Mat tmp;
	cv::resize(undistort, tmp, cv::Size(), 0.5, 0.5);
	cv::imshow("exo", tmp);
	cv::waitKey(100);

	registerRGBandPointCloud(exo_rgb_cam_->rgb_frame, scan_cloud, rgb_pose);

	if (pot_id < pot_labels.size())
	{
		std::string name = "Data\\" + pot_labels[pot_id];
		
		pcl::io::savePCDFileBinary(name + ".pcd", *scan_cloud);

		//vector<int> compression_params;
		//compression_params.push_back(CV_IMWRITE_PNG_COMPRESSION);
		//compression_params.push_back(9);
		//cv::imwrite(name + ".png", undistort, compression_params);

		cv::imwrite(name + ".bmp", undistort);
	}
#endif

#ifndef JUST_SCAN_A_POT
	//move above balance with camera facing down
	array6[0] = -0.020; array6[1] = 0.3458; array6[2] = 0.500;
	array6[3] = M_PI; array6[4] = 0; array6[5] = 0;
	robot_arm_client_->moveHandL(array6, 0.1, 0.1);
	robot_arm_client_->waitTillHandReachDstPose(array6);

	//switch gripper
	robot_arm_client_->getCurJointPose(array6);
	array6[4] = M_PI_2*3.;
	robot_arm_client_->moveHandJ(array6, move_joint_speed_, move_joint_acceleration_, true);

	//water
	double waterconfig[6] = {136.01, -49.07, -155.83, -64.96, 270.02, -45.88};
	for (int i = 0; i < 6; i++) waterconfig[i] = waterconfig[i] / 180.*M_PI;
	robot_arm_client_->moveHandJ(waterconfig, move_joint_speed_, move_joint_acceleration_, true);

	std::cout << "watering...\n";

	Sleep(5000);
//	getWater(5, true);

	robot_arm_client_->moveHandJ(array6, move_joint_speed_, move_joint_acceleration_, true);

	pump->getWeightClear(2);

	transportPotOnBalance(false);
#endif
}

void VisionArmCombo::placePots(int operation)
{
	if (operation != PICK_POT && operation != PLACE_POT)
	{
		std::cout << "invalid operation\n";
		return;
	}

	Eigen::Matrix4f cur_pose; getCurHandPose(cur_pose);

#if 0
	if (cur_pose(2, 2) >= 0.)	//check z direction
	{
		std::cout << "gripper pointing down!\n";
		return;
	}
#endif

	double array6[6];

	for (int cur_robot_stop = 0; cur_robot_stop < 3; cur_robot_stop++)
	{
#if 0
		// move robot to destination
		sendRoboteqVar(1, cur_robot_stop+1);	//roboteq start from 1

		std::cout << "sent\n";

		Sleep(1000);

		while (true)
		{
			int result = -1;
			int target_marker_id = -1;
			motor_controller_.GetValue(_VAR, 2, result);	//get current status
			motor_controller_.GetValue(_VAR, 1, target_marker_id);	//get current status
			std::cout << "status: "<< result <<" target marker:"<< target_marker_id<< "\n";
			if (result == 1) break;
			Sleep(500);
		}
#endif
		//std::cout << "Press key\n"; std::getchar(); continue;
		//1: right side of the robot; 0: left side
		//for (int cur_side = 1; cur_side >= 0; cur_side--)
		for (int cur_side = 0; cur_side >= 0; cur_side--)
		{
			viewer_->removeAllShapes();

#ifndef JUST_SCAN_A_POT
			if (localizeByScanSpheres(cur_robot_stop, cur_side))	//comment if only scan a pot
#endif
			{
				//continue;

				if (operation == PLACE_POT)	// move to operator
					array6[0] = cur_side == 0 ? 45 : 135;	
				else if (operation == PICK_POT) // move hand above table
					array6[0] = cur_side == 0 ? 0 : 180;
				array6[1] = -90;
				array6[2] = -90; array6[3] = -90;
				array6[4] = 90; array6[5] = 0;
				for (int i = 0; i < 6; i++)	array6[i] = array6[i] / 180.*M_PI;

				//disable if only scan a pot
#ifndef JUST_SCAN_A_POT	
				robot_arm_client_->moveHandJ(array6, move_joint_speed_, move_joint_acceleration_, true);

				// rotate gripper down
				array6[4] = M_PI_2*3;
				robot_arm_client_->moveHandJ(array6, move_joint_speed_, move_joint_acceleration_, true);
#endif

				int count = 0;

				for(int pot_id=0; pot_id<pot_map_.size(); pot_id++)
				{
					if (/*count%30 == 0 &&*/ pot_map_[pot_id].moisture >=0.f && pot_map_[pot_id].robot_stop == cur_robot_stop && pot_map_[pot_id].side == cur_side)
					{
						if (operation == PLACE_POT)
						{
							array6[0] = cur_side == 0 ? 45 : 135;
							array6[1] = -90;
							array6[2] = -90; array6[3] = -90;
							array6[4] = 270; array6[5] = 0;
							for (int i = 0; i < 6; i++)	array6[i] = array6[i] / 180.*M_PI;
							robot_arm_client_->moveHandJ(array6, move_joint_speed_, move_joint_acceleration_, true);

							gripper_.close();
							std::cout << "put in the pot\n";
							std::getchar();
							gotoPot(cur_robot_stop, cur_side, pot_map_[pot_id].local_pot_x, pot_map_[pot_id].local_pot_y, true);
						}
						else if (operation == PICK_POT)
						{

#ifndef JUST_SCAN_A_POT 
							gripper_.open();	//comment to just scan
#else
						//	gripper_.close(); std::getchar();
#endif

							Sleep(500);

#ifndef JUST_SCAN_A_POT 
							gotoPot(cur_robot_stop, cur_side, pot_map_[pot_id].local_pot_x, pot_map_[pot_id].local_pot_y, false); //comment to just scan
#endif	

							gotoBalance(pot_id);

#ifndef JUST_SCAN_A_POT 
							gotoPot(cur_robot_stop, cur_side, pot_map_[pot_id].local_pot_x, pot_map_[pot_id].local_pot_y, true); //comment to just scan
#endif
						}
					}

					count++;
				}
			}

			// rotate gripper up
			//above balance with gripper down
			array6[0] = 90; array6[1] = -90;
			array6[2] = -90; array6[3] = -90;
			array6[4] = 270; array6[5] = 0;
			for (int i = 0; i < 6; i++)	array6[i] = array6[i] / 180.*M_PI;
			robot_arm_client_->moveHandJ(array6, move_joint_speed_, move_joint_acceleration_, true);

			array6[4] = M_PI*0.5;
			robot_arm_client_->moveHandJ(array6, move_joint_speed_, move_joint_acceleration_, true);

			std::cout << "robot stop: " << cur_robot_stop << "   side: " << cur_side << "\n";
			//std::getchar();
		}
	}
}

void VisionArmCombo::initEXO_RGB_Cam()
{
	exo_rgb_cam_ = new EXO_RGB_CAM();
	exo_rgb_cam_->exposure_time_ = exposure_time_;
	exo_rgb_cam_->init();
}

void VisionArmCombo::initPotMap()
{
	pot_map_.resize(max_pots_);
	for (int i = 0; i<max_pots_; i++)
	{
		int x = i % table_cols_;
		int y = i / table_cols_;

		Pot* p = &pot_map_[i];
		p->moisture = 100.0f;

		if (y < 7)
		{
			p->robot_stop = 0;
			p->local_pot_y = y;
		}
		else if (y < 13)
		{
			p->robot_stop = 1;
			p->local_pot_y = y - 7 + 1;	// 6 rows in the stop 1
		}
		else
		{
			p->robot_stop = 2;
			p->local_pot_y = y - 13;
		}

		if (x < 6)
		{
			p->side = 1;
			p->local_pot_x = table_cols_ / 2 - 1 - x;
		}
		else
		{
			p->side = 0;
			p->local_pot_x = x - table_cols_ / 2;
		}
	}
}

int VisionArmCombo::getWater(float weight, bool b) { 
	int dig_id, ana_id, switch_id;
	float weight_th;
	int sleeptime;
	if (b) { //water
		dig_id = 7; ana_id = 0; weight_th = -1.1;
		switch_id = 5;//off-forward on-backward
		sleeptime = 500;
	}
	else {  //additive
		dig_id = 6; ana_id = 1; weight_th = -1.4;
		switch_id = 4;//on forward off-backward
		sleeptime = 1000;
	}

	robot_arm_client_->setDigitalOutput(dig_id, false); //brake
	Sleep(1000);

	pump->getWeightClear(2);
	Sleep(1000);
	//run
	if (b) {
		robot_arm_client_->setDigitalOutput(switch_id, false);
	}
	else {
		robot_arm_client_->setDigitalOutput(switch_id, true);
	}
	Sleep(100);
	robot_arm_client_->setAnalogOutput(ana_id, 0.3); Sleep(100);
	robot_arm_client_->setDigitalOutput(dig_id, true); 

	while (pump->getWeight(2) - weight < weight_th) {
		Sleep(10);
		std::cout << "water: " << pump->getWeight(2) << std::endl;
	}

	robot_arm_client_->setDigitalOutput(dig_id, false); //brake
	Sleep(sleeptime);  

	//feedback
	while (pump->getWeight(2) - weight < -0.1) { 
		robot_arm_client_->setAnalogOutput(ana_id, 0); Sleep(100);
		robot_arm_client_->setDigitalOutput(dig_id, true);
		Sleep(500);
		robot_arm_client_->setDigitalOutput(dig_id, false); //brake
		Sleep(500);
//		std::cout << "The weight is  " << pump->getWeight(2) << std::endl;
	}
	
	float temp = pump->getWeight(2);
	Sleep(1000);
	if (pump->getWeight(2) - temp > 0.1) {
		std::cout << "The pump hasn't stopped!" << std::endl;
		return -1;
	}

	//backward start
	cout << "starting backward!" << endl;
	if (b) {
		robot_arm_client_->setDigitalOutput(switch_id, true); 
	}
	else {
		robot_arm_client_->setDigitalOutput(switch_id, false); 
	}
	Sleep(100);
	robot_arm_client_->setAnalogOutput(ana_id, 0.3); Sleep(100);//////////////
	robot_arm_client_->setDigitalOutput(dig_id, true);
	Sleep(1500-sleeptime); //if true sleep 1000

	robot_arm_client_->setDigitalOutput(dig_id, false);
	Sleep(100);

	gripper_.open(0xF900);
	gripper_.close(0xF900);
	gripper_.open(0xF900);
}

void VisionArmCombo::EigenMatrix4dToCVMat4d(Eigen::Matrix4d & eigen_mat, cv::Mat & cv_mat)
{
	cv_mat.create(4, 4, CV_64F);
	for (int y = 0; y < 4; y++) {
		for (int x = 0; x < 4; x++) {
			cv_mat.at<double>(y, x) = eigen_mat(y, x);
		}
	}
}