#include "N-ScanHub.h"
#include <stdio.h>
#include <windows.h>
#include <chrono>
#include <vector>
#include <string>
#include <iostream>
#include <fstream>
#include <stdlib.h>
#include <thread>
#include "Debug.h"
using namespace std;

#define NET_TEST 1

// Read device data
void ReadCallback(const HANDLEDEV hDevice, const char* buf, int len)
{
	printf("-----------ReadCallback len=%d, buf=%s\n", len, buf);
}

// Monitoring device status change
void DevStatChangeCallback(const HANDLEDEV hDevice, bool isDevExisted)
{
	if (isDevExisted)
		printf("hDevice=%p, device is pushed in\n", hDevice);
	else
		printf("hDevice=%p, device is pushed out\n", hDevice);
}
#if NET_TEST
void TcpServiceBack(int clientSocket, char* clientIp) {
	printf("clientIp=%s\n", clientIp);
	char buf[4096] = { 0 };
	int len = 4096;
	while (1) {
		if (nl_readFromSocket(clientSocket, 2000, buf, &len) == 0) {
			printf("TcpServiceBack buf=%s\n", buf);
			memset(buf, 0, sizeof(buf));
		}
		Sleep(500);
	}
}

int piccount = 0;
void __stdcall readNetImage(unsigned char* data, int data_len) {
	char filename[256] = { 0 };
	sprintf(filename, "./pic/%d.jpg", piccount++);
	//nl_saveNetImgData(0, data, data_len, filename);
}

void __stdcall readTcpIpData(unsigned char* data, int data_len) {
	printf("data=%s\n", data);
}

void ServerThread() {
	int ret = nl_CreateTcpService(10000, TcpServiceBack);
	printf("ServerThread ret=%d\n", ret);
}

void NetImageThread(char* ip, int* port1, int* port2) {
	int socket36520 = -1;

	char sendbuf[1024] = { 0 };
	char recvbuf[1024] = { 0 };
	int socket30000 = -1;

	strcpy(sendbuf, "\x01\x54\x04");

	nl_connectToService(ip, *port1, &socket30000);
	nl_connectToService(ip, *port2, &socket36520);
	const int RECV_BUFFER_SIZE = 1920 * 1080 * 4;
	char* recvBuffer = (char*)malloc(RECV_BUFFER_SIZE);
	int realLen = 0, nRet = -1;
	IMG_TYPE imgtype;
	int w, h, f, q;
	f = 2;
	q = 2;
	char filename[128] = { 0 };

	for (int i = 0; i < 50000; i++) {
		//不建议在循环内部每次都连接设备关闭设备，频繁连接断开设备会读取失败
		//nl_connectToService(ip, *port1, &socket30000);
		//nl_connectToService(ip, *port2, &socket36520);
		//const int RECV_BUFFER_SIZE = 1920 * 1080 * 4;
		//char* recvBuffer = (char*)malloc(RECV_BUFFER_SIZE);
		//int realLen = 0, nRet = -1;
		//IMG_TYPE imgtype;
		//int w, h, f, q;
		//f = 2;
		//q = 2;
		//char filename[128] = { 0 };

		memset(recvBuffer, 0, RECV_BUFFER_SIZE);
		memset(recvbuf, 0, 1024);
		// rigger recognition
		if (nl_sendDataToSocket(socket30000, sendbuf, 3) != 0) {
			printf("nl_sendDataToSocket error\n");
			continue;
		}
		//get code
		if (nl_readFromSocket(socket30000, 2000, recvbuf, &realLen) != 0) {
			printf("nl_readFromSocket error\n");
			nl_CloseClientSocket(&socket30000);
			while (true) {
				Sleep(500);
				nl_connectToService(ip, *port1, &socket30000);
				if (socket30000 == -1) {
					printf("reconncet error\n");
					continue;
				}
				else
					break;
			}
			continue;
		}
		printf("code length=%d,data=%s\n", realLen, recvbuf);
	GET_IMAGE_TRY:
		realLen = RECV_BUFFER_SIZE;
		// In retrieving images, the two most commonly used parameters are 'f,' which represents the data type of the image, and 'q,' which is the compression quality level for obtaining JPEG image data when 'f' is set to 2
		nRet = nl_getNetImgData(socket36520, 0, 0, f, q, recvBuffer, &realLen, &imgtype, &w, &h);
		printf("-------------------------------------ip=%s [%d][%d]\n", ip, nRet, realLen);
		if (nRet != 0) {
			nl_CloseClientSocket(&socket36520);
			printf("close socket 36520\n");
			while (true) {
				Sleep(500);
				nl_connectToService(ip, *port2, &socket36520);
				if (socket36520 == -1) {
					printf("reconncet error\n");
					continue;
				}
				else
					break;
			}
			goto GET_IMAGE_TRY;
		}
			
		printf("nl_getNetImgData succ\n");
		// When 'f' is set to 0, it represents raw data, and you need to call the image-saving interface to package the raw data in order to generate an image file
		// If you prefer not to use the saving interface, you can also implement image data packaging on your own
		if (f == 0) {
			if (imgtype == TYPE_COLOR) {
				sprintf(filename, "./pic/f0-%s-%04d.bmp", ip, i);

				nl_SavePicDataToFile(filename, (unsigned char*)recvBuffer, w, h, 24); // Save image

				sprintf(filename, "./pic/f0-%s-%04d.jpg", ip, i);

				nl_SavePicDataToFile(filename, (unsigned char*)recvBuffer, w, h, 23); // Save image
				printf("nl_SavePicDataToFile jpg end");
			}
			else {
				sprintf(filename, "./pic/f0-%s-%04d.bmp", ip, i);
				nl_SavePicDataToFile(filename, (unsigned char*)recvBuffer, w, h, 8); // Save image
				sprintf(filename, "./pic/f0-%s-%04d.jpg", ip, i);
				nl_SavePicDataToFile(filename, (unsigned char*)recvBuffer, w, h, 13); // Save image
			}
		}
		// When 'f' takes any of the following values, you will obtain image format data that can be directly saved as a binary file
		else if (f == 1) {
			sprintf(filename, "./pic/f1-%s-%04d.bmp", ip, i);
			FILE* fp = fopen(filename, "wb");
			fwrite(recvBuffer, 1, realLen, fp);
			fclose(fp);
		}
		else if (f == 2) {
			sprintf(filename, "./pic/f2-%s-%05d.jpg", ip, i);
			FILE* fp = fopen(filename, "wb");
			fwrite(recvBuffer, 1, realLen, fp);
			fclose(fp);
		}
		else if (f == 3) {
			sprintf(filename, "./pic/f3-%s-%04d.bmp", ip, i);
			FILE* fp = fopen(filename, "wb");
			fwrite(recvBuffer, 1, realLen, fp);
			fclose(fp);
		}
		else if (f == 4) {
			sprintf(filename, "./pic/f4-%s-%04d.bmp", ip, i);
			FILE* fp = fopen(filename, "wb");
			fwrite(recvBuffer, 1, realLen, fp);
			fclose(fp);
		}
		//nl_CloseClientSocket(&socket36520);
		//nl_CloseClientSocket(&socket30000);
		//free(recvBuffer);
		////delete[]recvbuf;
		//recvBuffer = NULL;
		Sleep(50);
	}
	nl_CloseClientSocket(&socket36520);
	nl_CloseClientSocket(&socket30000);
	free(recvBuffer);
	//delete[]recvbuf;
	recvBuffer = NULL;
	printf("-------------------------------------ip=%s close\n", ip);
}
#endif

void SplitString(const string& s, vector<string>& v, const string& c)
{
	string::size_type pos1, pos2;
	pos2 = s.find(c);
	pos1 = 0;
	while (string::npos != pos2)
	{
		v.push_back(s.substr(pos1, pos2 - pos1));

		pos1 = pos2 + c.size();
		pos2 = s.find(c, pos1);
	}
	if (pos1 != s.length())
		v.push_back(s.substr(pos1));
}

int main(int argc, char* argv[])
{
#if NET_TEST
	//----------net test------------
	if (argc >= 2 && strcmp(argv[1], "--NetGetImg") == 0) {
		char ip1[20] = { 0 };
		char ip2[20] = { 0 };
		char ip3[20] = { 0 };
		int td1 = 1, td2 = 2;
		int port2 = 36520;
		int port1 = 30000;
		strcpy(ip1, "192.168.3.47");
		thread t1(NetImageThread, ip1, &port1, &port2);
		//strcpy(ip2, "192.168.3.219");
		//thread t2(NetImageThread, ip2, &port1, &port2);
		//strcpy(ip3, "192.168.3.197");
		//thread t3(NetImageThread, ip3, &port1, &port2);
		t1.join();
		//t2.join();
		//t3.join();
		return 0;
	}
	else if (argc >= 2 && strcmp(argv[1], "--ServerMode") == 0) {
		// The network server only provides a simple mode for reference, and it is recommended to implement the server mode according to specific requirements
		thread tt(ServerThread);
		tt.detach();
		Sleep(30000);
		nl_ExitTcpService();
		printf("exit\n");
		return 0;
	}
	else if (argc >= 2 && strcmp(argv[1], "--ClientMode") == 0) {
		int socket30000 = -1;
		nl_connectToService((char*)"192.168.76.96", 30000, &socket30000);
		char sendbuf[1024] = { 0 };
		char recvbuf[1024] = { 0 };
		int realLen = 0;
		memcpy(sendbuf, "\x01\x54\x04", 3);
		int count = 100;
		while (count--) {
			memset(recvbuf, 0, sizeof(recvbuf));
			int ret = nl_sendDataToSocket(socket30000, sendbuf, 3);
			printf("ret=%d\n", ret);

			//get code
			ret = nl_readFromSocket(socket30000, 2000, recvbuf, &realLen);
			if (ret != 0) {
				nl_CloseClientSocket(&socket30000);
				nl_connectToService((char*)"192.168.76.96", 30000, &socket30000);
			}
			printf("ret=%d recvbuf=%s\n", ret, recvbuf);
			Sleep(300);
		}

		nl_CloseClientSocket(&socket30000);
		return 0;
	}
	//----------net test over-------
#endif

	int deviceCounts = 0;
	HANDLEDEVLST hDeviceList = NULL;

	printf("enum nl_EnumDevices begin\n");
	//hDeviceList = nl_EnumDevices(&deviceCounts, ENUM_SERIAL|ENUM_SERIALUSB); // Enumerate device
	hDeviceList = nl_EnumDevices(&deviceCounts, ENUM_ALL); // Enumerate device
	printf("enum nl_EnumDevices end deviceCounts=%d\n", deviceCounts);

	if (argc >= 2 && strcmp(argv[1], "--BaseInfo") == 0) {
		if (deviceCounts == 0)
			return 0;
		printf("begin query base info\n");
		char receivedData[65565] = { 0 };
		int recvlen = 65535;
		nl_GetBaseInfo(hDeviceList, 0, receivedData, &recvlen);
		printf("base info:%s\n", receivedData);
		nl_ReleaseDevices(&hDeviceList);
		return 0;
	}
	for (int i = 0; i < deviceCounts; i++) // Get all device information
	{
		HANDLEDEV hDevice;
		hDevice = nl_OpenDevice(hDeviceList, i); // Open the device
		printf("hDevice=%p, %s\n", hDevice, hDevice != NULL ? "succeed in opening the device" : "failed to open the device");

		if (NULL == hDevice)
			continue;

		T_DeviceStatus status = nl_GetDevStatus(hDevice); // Get device status
		printf("status=%d\n", status);
		if (argc < 2) {
			// Write character string data
			// trigger recognition
			const char* strCmd = "QRYSYS"; // QRYSYS: System information
			char receivedData[65565] = { 0 };
			int recvlen = 65535;
			nl_Write(hDevice, strCmd, 6, true);
			int ret = nl_Read(hDevice, receivedData, 1000, 1000);
			printf("system info: \n%s\n", receivedData);
			//STDeviceInfo dInfo;
			//memset(&dInfo, 0, sizeof(STDeviceInfo));
			//nl_GetDeviceInfoByHandle(hDevice, &dInfo);
			//printf("type=[%d] info=[%s]\n", dInfo.devType, dInfo.devInfo);
		}

		if (argc >= 2 && strcmp(argv[1], "--WriteAsHex") == 0) // Write data to the device in HEX character string
		{
			// Write hex character string data
			const char* strCmdhEX = "7e 01 30 30 30 30 40 51 52 59 53 59 53 3b 03"; // System information
			char receivedData[1024] = { 0 };
			int nRet = 0;
			bool isWrited = nl_WriteAsHex(hDevice, strCmdhEX, true); // Write data
			nRet = nl_Read(hDevice, receivedData, sizeof(receivedData), 0); // Read data
			printf("nRet=%d, receivedData=%s\n", nRet, receivedData);
		}
		else if (argc >= 2 && strcmp(argv[1], "--GetDeviceInfo") == 0) // Write data to the device in HEX character string
		{
			// Retrieve device information
			STDeviceInfo info;
			memset(&info, 0, sizeof(STDeviceInfo));
			nl_GetDeviceInfo(hDeviceList, i, &info);
			printf("GetDeviceInfo -------- info\n %s\ntype=%d\n", info.devInfo, info.devType);
			//memset(&info, 0, sizeof(STDeviceInfo));
			//nl_GetDeviceInfoByHandle(hDevice, &info);
			//printf("GetDeviceInfoByHandle -------- info\n %s\ntype=%d\n", info.devInfo, info.devType);
		}
		else if (argc >= 2 && strcmp(argv[1], "--SendCommand") == 0) // Send control commands to the device and obtain the returned information
		{
			// Send a command and check if the command was successful.
			char strCmd[2048] = { 0 };
			strcpy(strCmd, "QRYSYS");
			int result = nl_SendCommand(hDevice, strCmd, strlen(strCmd)); // Send commands
			printf("result=%d\n", result);
		}
		else if (argc >= 2 && strcmp(argv[1], "--SendCommandAsHex") == 0) // Send control commands to the device in the form of HEX character string and get the returned information.
		{
			const char* strCmd = "51 52 59 53 59 53"; // QRYSYS: System information
			T_CommunicationResult result = nl_SendCommandAsHex(hDevice, strCmd, strlen(strCmd)); // Send commands
			printf("result=%d\n", result);
		}
		else if (argc >= 2 && strcmp(argv[1], "--GetCommandResponse") == 0) {
			// Send a command, receive the command returned by the device, with a maximum supported length of 20,000.
			//nl_SetListener(hDevice, ReadCallback);
			//Sleep(1000);
			char strCmd[20480] = { 0 };
			strcpy(strCmd, "\x3f");
				char recvData[20480] = { 0 };
				int recvLen = 20480;
				bool result = nl_GetCommandResponse(hDevice, strCmd, strlen(strCmd), recvData, &recvLen, 2000, false, false);
				printf("result=%d,recvLen=%d\n", result, recvLen);
				printf("recvData=%s\n", recvData);
			//nl_StopListener(hDevice);
		}
		else if (argc >= 2 && strcmp(argv[1], "--GetPicture") == 0) // Get device image
		{
			// Basic interface for obtaining images, with all parameters set to default.
			unsigned int imgWidth = 0, imgHeight = 0;
			char filename[1024] = { 0 };
			sprintf(filename, "1%d.bmp", i);
			bool isGetPicSizeOK = nl_GetPicSize(hDevice, &imgWidth, &imgHeight); // Get the image width and height
			printf("nl_GetPicSize isGetPicSizeOK=%d\n", isGetPicSizeOK);
			if (isGetPicSizeOK && imgWidth > 0 && imgHeight > 0)
			{
				printf("imgWidth=%d,imgHeight=%d\n", imgWidth, imgHeight);
				const int RECV_BUFFER_SIZE = imgWidth * imgHeight;
				unsigned char* recvBuffer = (unsigned char*)malloc(RECV_BUFFER_SIZE);
				bool isOK = nl_GetPicData(hDevice, recvBuffer, RECV_BUFFER_SIZE); // Get the image raw data
				if (isOK)
					nl_SavePicDataToFile(filename, recvBuffer, imgWidth, imgHeight, 8);
			}
		}
		else if (argc >= 2 && strcmp(argv[1], "--GetPictureByConfig") == 0) // Get device image
		{
			// Obtain image data with configurable parameters.
			unsigned int imgWidth = 0, imgHeight = 0;
			bool isGetPicSizeOK = nl_GetPicSize(hDevice, &imgWidth, &imgHeight); // Get the image width and height
			printf("nl_GetPicSize isGetPicSizeOK=%d\n", isGetPicSizeOK);
			if (isGetPicSizeOK && imgWidth > 0 && imgHeight > 0)
			{
				printf("imgWidth=%d,imgHeight=%d\n", imgWidth, imgHeight);
				const int RECV_BUFFER_SIZE = imgWidth * imgHeight * 4;
				// Allocate a sufficiently large space to store image data
				unsigned char* recvBuffer = (unsigned char*)malloc(RECV_BUFFER_SIZE);
				STImgParam imgParam;
				memset(&imgParam, 0, sizeof(STImgParam));
				imgParam.f = 2;
				imgParam.q = 0;
				STImgResolution imgR[4];
				memset(imgR, 0, sizeof(STImgResolution) * 4);
				int j = 1;
				while (j++) {
					memset(recvBuffer, 0, RECV_BUFFER_SIZE);
					unsigned int nRealLen = RECV_BUFFER_SIZE;
					bool isOK = nl_GetPicDataByConfig(hDevice, imgParam, recvBuffer, &nRealLen, imgR); // Get the image data
					printf("isOK=%d\n", isOK);
					char filename[1024] = { 0 };
					if (isOK) {
						if (imgParam.t == 2) {
							for (int x = 0; x < 4; x++) {
								printf("imgR[%d] width=%d height=%d\n", x, imgR->width, imgR->height);
							}
						}
						if (imgParam.f == 1) {
							sprintf(filename, "test3%d.bmp", i);
							FILE* fp = fopen(filename, "wb");
							fwrite(recvBuffer, 1, nRealLen, fp);
							fclose(fp);
						}
						else if (imgParam.f == 2) {
							sprintf(filename, "./pic/test4%d%06d.jpg", i, j);
							FILE* fp = fopen(filename, "wb");
							fwrite(recvBuffer, 1, nRealLen, fp);
							fclose(fp);
						}
						else if (imgParam.f == 3) {
							sprintf(filename, "test5%d.tiff", i);
							FILE* fp = fopen(filename, "wb");
							fwrite(recvBuffer, 1, nRealLen, fp);
							fclose(fp);
						}
						else if (imgParam.f == 4) {
							sprintf(filename, "test6%d.bmp", i);
							FILE* fp = fopen(filename, "wb");
							fwrite(recvBuffer, 1, nRealLen, fp);
							fclose(fp);
						}
						else if (imgParam.f == 0) {
							long outLen = 0;
							STImgResolution imgResIn, imgResOut;
							if (imgParam.r == 1) {
								imgWidth = imgWidth / 2;
								imgHeight = imgHeight / 2;
							}
							else if (imgParam.r == 2) {
								imgWidth = imgWidth / 4;
								imgHeight = imgHeight / 4;
							}
							imgResIn.width = imgWidth;
							imgResIn.height = imgHeight;
							unsigned int imgLen = 0;
							IMG_TYPE type = nl_GetDeviceImageColorType(hDevice, &imgResOut, &imgLen);
							if (type == TYPE_COLOR) {
								unsigned char* outBuf = (unsigned char*)malloc(imgLen);
								printf("imgLen=%d\n", imgLen);
								bool res = nl_ConvertImageColorSpace(hDevice, recvBuffer, RECV_BUFFER_SIZE, imgResIn, outBuf);
								int oWidth, oHeight;
								// If you are capturing a partial image, use the resolution of the captured portion
								if (strlen(imgParam.b) != 0) {
									oWidth = stoi(string(imgParam.b).substr(8, 4));
									oHeight = stoi(string(imgParam.b).substr(12, 4));
								}
								else {
									oWidth = imgResOut.width;
									oHeight = imgResOut.height;
								}
								sprintf(filename, "./pic/test2%d.bmp", i);
								nl_SavePicDataToFile(filename, outBuf, oWidth, oHeight, 24); // Save image
								sprintf(filename, "./pic/test2%d.jpg", i);
								nl_SavePicDataToFile(filename, outBuf, oWidth, oHeight, 23); // Save image
							}
							else {
								int oWidth, oHeight;
								if (strlen(imgParam.b) != 0) {
									oWidth = stoi(string(imgParam.b).substr(8, 4));
									oHeight = stoi(string(imgParam.b).substr(12, 4));
								}
								else {
									oWidth = imgResOut.width;
									oHeight = imgResOut.height;
									if (imgParam.r == 1) {
										oWidth = imgResOut.width / 2;
										oHeight = imgResOut.height / 2;
									}
									else if (imgParam.r == 2) {
										oWidth = imgResOut.width / 4;
										oHeight = imgResOut.height / 4;
									}
								}
								sprintf(filename, "./pic/test1%d.bmp", i);
								nl_SavePicDataToFile(filename, recvBuffer, oWidth, oHeight, 8); // Save image
								sprintf(filename, "./pic/test1%d.jpg", i);
								nl_SavePicDataToFile(filename, recvBuffer, oWidth, oHeight, 13); // Save image
							}
						}
					}
				}
				free(recvBuffer);
				recvBuffer = NULL;
			}
		}
		else if (argc >= 2 && strcmp(argv[1], "--SetListener") == 0) // Asynchronous reading of device data
		{
			// Barcode scanning listener, when activated, the device will pass the scanned code to the callback function ReadCallback
			nl_SetListener(hDevice, ReadCallback);
			Sleep(1000000);
			nl_StopListener(hDevice);
		}
		else if (argc >= 2 && strcmp(argv[1], "--ReadDevCfgToXml") == 0) // Read the configuration from the device and save it to the xml file.
		{
			char filename[128] = { 0 };
			strcpy(filename, "./xml/2.xml");
			nl_ReadDevCfgToXml(hDevice, filename);
		}
		else if (argc >= 2 && strcmp(argv[1], "--WriteCfgToDev") == 0) // Write the configuration file information to the device.
		{
			bool ret = nl_WriteCfgToDev(hDevice, "./xml/2.xml");
			if (!ret)
				printf("WriteCfgToDev %s\n", nl_GetLastError());//If there is an error during XML import, calling nl_GetLastError can provide information about which commands were imported incorrectly
		}
		else if (argc >= 2 && strcmp(argv[1], "--SetCbDevStatusChanged") == 0) // Set the callback function when the device status changes
		{
			// Monitor device plug and unplug states and take actions to open or close, only monitor local serial and USB devices, this interface is not effective for network devices
			nl_SetCbDevStatusChanged(hDevice, DevStatChangeCallback);
			while (true) {
				char receivedData[1024] = { 0 };
				int nRet = nl_Read(hDevice, receivedData, sizeof(receivedData), 1000); // Read data
				if(nRet > 0)
					printf("nRet=%d, receivedData=%s\n", nRet, receivedData);
			}
			printf("SetCbDevStatusChanged finish\n");
		}
		else if (argc >= 2 && strcmp(argv[1], "--UpdateFirmware") == 0) // Update device
		{
			unsigned updateError = -1;
			bool isUpdated = nl_UpdateKernelDevice(hDevice, "Y:/Newland/scan/firmware/FM600/FM600_system_lsc_20.bin", 0, &updateError); // Firmware update
			printf("updateError=%d,%s\n", updateError, isUpdated ? "succeed in updating the firmware" : "failed to update the firmware");
			switch (updateError)
			{
			case Success:
				printf("The firmware update is normal.\n");
				break;
			case FileNameExtError:
				printf("file name error\n");
				break;
			}
		}
		else if (argc >= 2 && strcmp(argv[1], "--SetNetDeviceConfig") == 0) {
			char configData[2048] = { 0 };
			strcpy(configData, "Serial Number=A6516268F31B66FC;MAC Address=00:51:62:68:F3:1B;Device Use DHCP=1;Device IP Address=192.168.76.250;Device SubNetmask=255.255.255.0;Device Gateway Address=192.168.76.1;");
			char outData[2048] = { 0 };
			int nRet = nl_SetNetDeviceConfig(NET_SETTING_TYPE::DEV_SETTING, configData, strlen(configData), 5000, outData);
			if (nRet != 0)
			{
				printf("nl_setNetDeviceConfig error\n");
			}
			printf("\n nl_setNetDeviceConfig outData=%s\n", outData);
		}
		else if (argc >= 2 && strcmp(argv[1], "--SendBmpDecode") == 0) {
			char filename[128] = { 0 };
			strcpy(filename, "decode.bmp");
			char receivedData[4096] = { 0 };
			int recvLen = 0;
			int decodeTime = 0;
			//nl_SendBmpDecode(hDevice, IMG_TYPE::TYPE_COLOR, filename, receivedData, &recvLen, &decodeTime, 2000);
		}
		else if (argc >= 2 && strcmp(argv[1], "--GetRoiPicData") == 0) {
			char filename[128] = { 0 };
			strcpy(filename, "GetRoiPicData.bmp");
			const int RECV_BUFFER_SIZE = 1920 * 1800 * 3;
			// Allocate a sufficiently large space to store image data
			unsigned char* receivedData = (unsigned char*)malloc(RECV_BUFFER_SIZE);
			memset(receivedData, 0, sizeof(receivedData));
			unsigned int recvLen = 1920 * 1800 * 3;

			STRoiImgParam param;
			memset(&param, 0, sizeof(STRoiImgParam));
			param.p = 0;
			param.u = 1;
			param.i = 1;
			param.r = 0;
			param.f = 0;
			param.q = 0;
			nl_GetRoiPicData(hDevice, param, receivedData, &recvLen);
			FILE* fp = fopen(filename, "wb");
			fwrite(receivedData, 1, recvLen, fp);
			fclose(fp);
			free(receivedData);
			receivedData = NULL;
		}

		bool isClosed = nl_CloseDevice(&hDevice); // Close the device
		printf("hDevice=%p,%s\n", hDevice, isClosed ? "succeed in closing the device" : "failed to close the device");
		T_DeviceStatus t = nl_GetDevStatus(hDevice);
		printf("T_DeviceStatus t=%d\n", t);
	}
	nl_ReleaseDevices(&hDeviceList); // Release the device list handle
	Sleep(1000);


	// Network devices can be asynchronously refreshed in the background
	//if (argc >= 2 && strcmp(argv[1], "--EnumNetDevAsyn") == 0) {
	//	printf("begin nl_BeginEnumNetDevice\n");
	//	nl_BeginEnumNetDevice();
	//	for (int i = 0; i < 16; i++) {
	//		hDeviceList = nl_EnumDevices(&deviceCounts);
	//		printf("------------asyn enum deviceCounts------------=[%d]\n", deviceCounts);
	//		Sleep(1000);
	//	}
	//	nl_StopEnumNetDevice();
	//	return 0;
	//}


	printf("all over\n");
	system("pause");

	return 0;
}
