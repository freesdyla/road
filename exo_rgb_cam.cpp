#include "exo_rgb_cam.h"

unsigned char* EXO_RGB_CAM::rgb_buffer_ = new unsigned char[1920*1200*3];

bool EXO_RGB_CAM::image_transfer_done;

cv::Mat EXO_RGB_CAM::rgb_frame;

std::mutex RGBMutex;

EXO_RGB_CAM::EXO_RGB_CAM()
{
	rgb_frame.create(1200, 1920, CV_8UC3);
}

EXO_RGB_CAM::~EXO_RGB_CAM()
{
	
}

int EXO_RGB_CAM::init()
{
	printf("SVU3V-SDK Mini Sample\n"); printf("\n");
	//---------------------------------------------------------------------
	// Discovery
	//---------------------------------------------------------------------

	int Index = 0;
	
	CameraClient = CameraContainer_create();

	if (CameraClient < 0)
	{
		printf("Create CameraContainer  faild !!\n");
		
		return 0;
	}

	CameraContainer_discovery(CameraClient);
	int NumCameras = 0;
	NumCameras = CameraContainer_getNumberOfCameras(CameraClient);
	if (NumCameras < 1)
	{
		printf("no cameras found !!\n");
		return 0;
	}
	printf("Nbr of Cameras found:  %-10d \n", NumCameras);
	// select the first discovered camera
	Camera = CameraContainer_getCamera(CameraClient, Index);
	std::string modelname = Camera_getModelName(Camera);
	printf("Selected Camera: %s \n", modelname.c_str());

	SVU3V_RETURN result;
	result = Camera_openConnection(Camera, 3.0);

	if (result != U3V_STATUS_SUCCESS)
	{
		printf("Open connection failed,  is camera occupied ?  \n");
		return -1;
	}
///	Camera_getSizeX(Camera, &SizeX);
//	Camera_getSizeY(Camera, &SizeY);
	Camera_getImageSize(Camera, &ImageSize);
//	std::cout << SizeX << " " << SizeY <<" "<<ImageSize<< "\n";

	//-----------------------------------------------------------------------

	float red = 0;
	float green = 0;
	float blue = 0;
	Camera_getWhiteBalance(Camera, &red, &green, &blue);

//	std::cout << "r: " << red << "g: " << green << "b: " << blue << "\n";

	Camera_setWhiteBalance(Camera, 155.078f, 100.f, 214.063f);

	Camera_setFlippingMode(Camera, SVU3V_REVERSE_X_Y);


	int BufferCount = 3;

	result = StreamingChannel_create(&StreamingChannel, CameraClient, Camera, BufferCount, &ImageCallback, 0);

	//---------------------------------------------------------------------
	// do some camera configurations
	//---------------------------------------------------------------------

	//------------------------------------------------------------------------
	Camera_setAcquisitionMode(Camera, ACQUISITION_MODE_SOFTWARE_TRIGGER);
	float  exposure = 50000; // 50 ms
	Camera_setExposureTime(Camera, exposure);
	float Gain = 0; // 0 db
	Camera_setGain(Camera, Gain);
	/* if the  settings (binning mode, AOI, pixel depth, Flipping mode) have been changed
	after creating Streaming channel, a reopen of streaming channel is required: */

	//---------------------------------------------------------------------
	// start Acquisition: Software Trigger
	//---------------------------------------------------------------------

	Camera_setAcquisitionControl(Camera, ACQUISITION_CONTROL_START);

//	Sleep(1000);

//	acquireRGBImage();

	return 0;
}

void EXO_RGB_CAM::stop()
{
	//---------------------------------------------------------------------
	// close Camera and and unregister messages
	//---------------------------------------------------------------------
	printf("Stop Acquisition...\n");
	Camera_setAcquisitionControl(Camera, ACQUISITION_CONTROL_STOP);
	printf("Delete streaming channel...\n");
	StreamingChannel_delete(StreamingChannel);
	printf("Close connection...\n");
	Camera_closeConnection(Camera);
	printf("Press any key to end...");
}

void EXO_RGB_CAM::acquireRGBImage()
{
	Camera_softwareTrigger(Camera);
	image_transfer_done = false;

	while (!image_transfer_done)
	{
		Sleep(10);
	}
}




// stream callback function will be registered when creating streaming channel
//--------------------------------------------------------------------------------------------
SVU3V_RETURN __stdcall EXO_RGB_CAM::ImageCallback(Image_handle Image, void* Context)
{
	//unsigned char *img_buffer_ = Image_getDataPointer(Image);
//	int success = Image_getImageRGB(Image, rgb_buffer_, SizeX*SizeY*3, BAYER_METHOD_HQLINEAR);RGBMutex
//	RGBMutex.lock();
	int success = Image_getImageRGB(Image, rgb_frame.ptr<uchar>(0), SizeX*SizeY*3, BAYER_METHOD_HQLINEAR);
	//success = Image_getImageRGB(Image, rgb_frame.ptr<uchar>(0), SizeX*SizeY * 3, BAYER_METHOD_HQLINEAR);
//	RGBMutex.lock();

//	std::cout << "return " << success << " ";

	if (success == 0) image_transfer_done = true;
	
//success = SVU3V_writeImageToBitmapFile("img.bmp", rgb_frame.ptr<uchar>(0), SizeX, SizeY, GVSP_PIX_RGB24);
//	success = SVU3V_writeImageToBitmapFile("img.bmp", rgb_buffer_, SizeX, SizeY, GVSP_PIX_RGB24);
	
	//printf("Callback:  %08x\n", *img_buffer_);
	//std::cout << "return " << success << "\n";
	return U3V_STATUS_SUCCESS;
}

