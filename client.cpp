/*
	Original author of the starter code
    Tanzir Ahmed
    Department of Computer Science & Engineering
    Texas A&M University
    Date: 2/8/20
	
	Please include your Name, UIN, and the date below
	Name: Ahmed Albsharat
	UIN: 934004488
	Date: 09-28-2025
*/
#include "common.h"
#include "FIFORequestChannel.h"

#include <sys/wait.h> // for wait
#include <unistd.h>  // for fork, exec
#include <fstream>   // for file 
#include <sys/stat.h> // for mkdir
#include <vector>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <thread>   // for sleep
#include <chrono>   // for sleep

using namespace std;

int main (int argc, char *argv[]) {
	int opt;
	int p = 1;
	double t = 0;
	int e = 1;
	string filename = "";

	vector<FIFORequestChannel*> channels;
	bool c = false;

	bool pFlag = false;
	bool tFlag = false;
	bool eFlag = false;
	bool fFlag = false;

	// parse options
	while ((opt = getopt(argc, argv, "p:t:e:f:c")) != -1) {
		switch (opt) {
			case 'p':
				p = atoi(optarg);
                pFlag = true;
				break;
			case 't':
				t = atof(optarg);
                tFlag = true;
				break;
			case 'e':
				e = atoi(optarg);
                eFlag = true;
				break;
			case 'f':
				filename = optarg;
                fFlag = true;
				break;
			case 'c':
				c = true;
				break;
			default:
				cerr << "Unknown option: " << char(opt) << endl;
				return 1;
		}
	}

	// start server child process
	pid_t pid = fork();
	if(pid < 0){
		cerr << "fork failed: " << strerror(errno) << endl;
		return 1;
	}
	if (pid == 0) {
		// child: replace with server executable
		char* args[] = {(char*)"./server", nullptr};
		execvp(args[0], args);
		// if execvp returns, it failed
		cerr << "exec failed: " << strerror(errno) << endl;
		_exit(1);
	}

	// parent
	// create initial control channel, retry until server is ready
	FIFORequestChannel* control = nullptr;
	for (int i = 0; i < 50 && !control; i++) { // try up to 50 times (~5s)
		try {
			control = new FIFORequestChannel("control", FIFORequestChannel::CLIENT_SIDE);
		} catch (...) {
			this_thread::sleep_for(chrono::milliseconds(100));
		}
	}
	if (!control) {
		cerr << "Could not connect to server control channel." << endl;
		waitpid(pid, nullptr, 0);
		return 1;
	}
	channels.push_back(control);

	// optionally create a new channel (server will return a channel name)
	if(c){
		MESSAGE_TYPE newChan = NEWCHANNEL_MSG;
		channels[0]->cwrite(&newChan, sizeof(MESSAGE_TYPE));

		// the server will write back the name of the new channel
		char name[100]; 
		memset(name, 0, sizeof(name));
		channels[0]->cread(name, sizeof(name));

		// construct new channel using returned name
		channels.push_back(new FIFORequestChannel(name, FIFORequestChannel::CLIENT_SIDE));
		cout << "New channel created: " << name << endl;
	}

	FIFORequestChannel* currChan = channels.back(); 
	if (!currChan) {
		cerr << "No communication channel available." << endl;
		// cleanup
		MESSAGE_TYPE m = QUIT_MSG;
		for(auto ch : channels){
			if (ch) ch->cwrite(&m, sizeof(MESSAGE_TYPE));
			delete ch;
		}
		waitpid(pid, nullptr, 0);
		return 1;
	}

	// ensure 'received' directory exists
	struct stat st{};
	if (stat("received", &st) == -1) {
		if (mkdir("received", 0755) == -1) {
			cerr << "Could not create 'received' directory: " << strerror(errno) << endl;
		}
	}

	// Case 1: only -p provided (produce CSV of 1000 samples)
	if(pFlag && !tFlag && !eFlag && !fFlag){
		ofstream outputFile("received/x1.csv");
		if(!outputFile){
			cerr << "Could not open received/x1.csv for writing." << endl;
		} else {
			for(int i = 0; i < 1000; i++){
				double time = i * 0.004;

				char buf[MAX_MESSAGE];
				datamsg x(p, time, 1); // ecg 1 
				memcpy(buf, &x, sizeof(datamsg));
				currChan->cwrite(buf, sizeof(datamsg));
				double ecg1;
				currChan->cread(&ecg1, sizeof(double));

				datamsg y(p, time, 2); // ecg 2
				memcpy(buf, &y, sizeof(datamsg));
				currChan->cwrite(buf, sizeof(datamsg));
				double ecg2;
				currChan->cread(&ecg2, sizeof(double));

				outputFile << time << "," << ecg1 << "," << ecg2 << endl;
			}
			outputFile.close();
			cout << "Data for person " << p << " written to received/x1.csv" << endl;
		}
	}
	// Case 2: only -f provided (download file)
	else if(!pFlag && !tFlag && !eFlag && fFlag){
		filemsg fm(0, 0);
		int len = sizeof(filemsg) + (filename.size() + 1);
		vector<char> buf2(len);
		memcpy(buf2.data(), &fm, sizeof(filemsg));
		strcpy(buf2.data() + sizeof(filemsg), filename.c_str());

		currChan->cwrite(buf2.data(), len);   // request file size
		
		__int64_t filesize = 0;
		currChan->cread(&filesize, sizeof(__int64_t));  

		if(filesize <= 0){
			cerr << "Server reports file size <= 0 or file not found for '" << filename << "'" << endl;
		} else {
			string dest = "received/" + filename;
			ofstream ofs(dest, ios::binary);
			if(!ofs){
				cerr << "Could not open file " << dest << " for writing." << endl;
			} else {
				const int chunk = MAX_MESSAGE;
				vector<char> recvbuf(chunk);
				__int64_t bytesReceived = 0;

				while(bytesReceived < filesize){
					int reqSize = (int)min((__int64_t)chunk, filesize - bytesReceived);
					filemsg fm2(bytesReceived, reqSize);
					int len2 = sizeof(filemsg) + (filename.size() + 1);

					if ((int)buf2.size() < len2) buf2.assign(len2, 0);
					memcpy(buf2.data(), &fm2, sizeof(filemsg));
					strcpy(buf2.data() + sizeof(filemsg), filename.c_str());

					currChan->cwrite(buf2.data(), len2);
					currChan->cread(recvbuf.data(), reqSize);

					ofs.write(recvbuf.data(), reqSize);
					bytesReceived += reqSize;

					if(bytesReceived % (chunk * 10) == 0 || bytesReceived == filesize){
						cout << "Received " << bytesReceived << " of " << filesize << " bytes" << endl;
					}
				}

				ofs.close();
				cout << "File '" << filename << "' received from server and stored in '" << dest << "'" << endl;
			}
		}
	}
	// Case 3: a query (p, t, e) or some combination (the default single query)
	else {
		char buf[MAX_MESSAGE];
    	datamsg x(p, t, e);
		memcpy(buf, &x, sizeof(datamsg));
		currChan->cwrite(buf, sizeof(datamsg));
		double reply;
		currChan->cread(&reply, sizeof(double));
		cout << "For person " << p << ", at time " << t << ", the value of ecg " << e << " is " << reply << endl;
	}

	// closing the channel    
    MESSAGE_TYPE m = QUIT_MSG;
    for(auto ch : channels){
		if (ch) ch->cwrite(&m, sizeof(MESSAGE_TYPE));
		delete ch;
	}
	channels.clear();

	// wait for the server to finish 
	waitpid(pid, nullptr, 0);
	return 0;
}