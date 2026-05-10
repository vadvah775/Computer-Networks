all: server client

server: server.cpp
	g++ -Wall -o server server.cpp -I/usr/include/jsoncpp -ljsoncpp -pthread

client: client.cpp
	g++ -Wall -o client client.cpp -I/usr/include/jsoncpp -ljsoncpp -pthread

clean:
	rm -f server client