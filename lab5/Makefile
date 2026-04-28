all: server client

server: server.cpp
	g++ -Wall -o server server.cpp -I/usr/include/jsoncpp -ljsoncpp
client: client.cpp
	g++ -Wall -o client client.cpp

clean:
	rm -f server client
