/*
 *  Copyright Beijing 58 Information Technology Co.,Ltd.
 *
 *  Licensed to the Apache Software Foundation (ASF) under one
 *  or more contributor license agreements.  See the NOTICE file
 *  distributed with this work for additional information
 *  regarding copyright ownership.  The ASF licenses this file
 *  to you under the Apache License, Version 2.0 (the
 *  "License"); you may not use this file except in compliance
 *  with the License.  You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing,
 *  software distributed under the License is distributed on an
 *  "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *  KIND, either express or implied.  See the License for the
 *  specific language governing permissions and limitations
 *  under the License.
 */
/*
 * DataReceiver.cpp
 *
 * Created on: 2011-7-5
 * Author: Service Platform Architecture Team (spat@58.com)
 */
#include "CSocket.h"
#include "DataReceiver.h"
#include "../threadpool/threadpool.h"
#include "CSocket.h"
#include "../protocol/ProtocolConst.h"
#include "SocketPool.h"
#include <sys/epoll.h>
#include <pthread.h>
namespace gaea {
DataReceiver* DataReceiver::dataReceiver = NULL;
pthread_mutex_t DataReceiver::mutex = PTHREAD_MUTEX_INITIALIZER;
std::map<int, SocketPool*>* DataReceiver::socketMap = new std::map<int, SocketPool* >;
typedef struct {
	int fd;
	int len;
	char* data;
}readDataStruct;
void DataReceiver::socketReadData(void *arg) {
	readDataStruct *rds = (readDataStruct*) (arg);
	CSocket::frameHandle(rds->fd, rds->data, rds->len);
	delete rds;
}

void *DataReceiver::epollReadData(void *arg) {
	struct epoll_event ev_read[10];
	int nfds = 0; //return the events count
	ThreadPool *tp = threadpool_create(5, 10240);
	int recBufferLen = 1024;
	char *recData = (char*) (malloc(recBufferLen));
	int start = 0, end = 0, totleLen = 0;
	int index = 0;
	int dataLen;
	int readLen = 0;
	SocketPool *sp = NULL;
	std::map<int, SocketPool*>::iterator it;
	while (1) {
		nfds = epoll_wait(*(int*) (arg), ev_read, 10, 300);
		for (int i = 0; i < nfds; ++i) {
			start = 0;
			end = 0;
			totleLen = 0;
			readData: readLen = CSocket::anetRead(ev_read[i].data.fd, recData + end, recBufferLen - end);
			if (readLen <= 0) {
				pthread_mutex_lock(&mutex);
				it = socketMap->find(ev_read[i].data.fd);
				if (it != socketMap->end()) {
					sp = it->second;
				}
				pthread_mutex_unlock(&mutex);
				if (sp) {
					sp->closeSocket(ev_read[i].data.fd);
					sp = NULL;
				}
				gaeaLog(GAEA_WARNING, "DataReceiver::anetRead,happened socket error%d,%d\n", end, recBufferLen);
				continue;
			}
			totleLen += readLen;
			while (end < totleLen) {
				if (recData[end] == P_END_TAG[index]) {
					++index;
					if (index == sizeof(P_END_TAG)) {
						dataLen = end - start - sizeof(P_START_TAG);
						char *retData = (char*) (malloc(dataLen));
						memcpy(retData, recData + start + sizeof(P_START_TAG), dataLen);
						readDataStruct *rds = new readDataStruct;
						rds->fd = ev_read[i].data.fd;
						rds->len = dataLen;
						rds->data = retData;
						threadpool_add_task(tp, socketReadData, rds);
						index = 0;
						++end;
						start = end;
					}
				} else if (recData[end] == P_END_TAG[0]) {
					index = 1;
				} else {
					index = 0;
				}
				++end;
			}

			if (end == totleLen) {
				if (start == 0 && end >= recBufferLen) {
					recData = (char*) (realloc(recData, recBufferLen * 2));
					recBufferLen = recBufferLen * 2;
					goto readData;
				} else if (start < end - 1) {
					memcpy(recData, recData + start, end - start);
					end -= start;
					start = 0;
					totleLen = end;
					goto readData;
				}
			}
		}

	}

	return NULL;
}

DataReceiver::DataReceiver() {
	epfd = epoll_create(1024);
	pthread_create(&ntid, NULL, epollReadData, &epfd);
}

void DataReceiver::registerSocket(int fd, SocketPool *socketPool) {
	epoll_event ev;
	ev.data.fd = fd;
	ev.events = EPOLLIN | EPOLLERR | EPOLLHUP;
	epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
	pthread_mutex_lock(&mutex);
	socketMap->insert(std::pair<int, SocketPool*>(fd, socketPool));
	pthread_mutex_unlock(&mutex);
}

void DataReceiver::unRegisterSocket(int fd) {
	epoll_event ev;
	ev.data.fd = 0;
	ev.events = EPOLLIN;
	epoll_ctl(epfd, EPOLL_CTL_DEL, fd, &ev);
	pthread_mutex_lock(&mutex);
	socketMap->erase(fd);
	pthread_mutex_unlock(&mutex);
}

DataReceiver *DataReceiver::GetInstance() {
	if (dataReceiver == NULL) {
		pthread_mutex_lock(&mutex);
		if (dataReceiver == NULL) {
			dataReceiver = new DataReceiver();
		}
		pthread_mutex_unlock(&mutex);
	}
	return dataReceiver;
}

DataReceiver::~DataReceiver() {
	delete dataReceiver;
}

int DataReceiver::getEpfd() const {
	return epfd;
}

}
