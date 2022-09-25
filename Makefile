a.out:src/http_conn.cpp src/main.cpp src/fd_operate.cpp
	g++ src/http_conn.cpp src/main.cpp src/fd_operate.cpp -pthread -Iinclude
