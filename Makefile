a.out:http_conn.cpp main.cpp fd_operate.cpp
	g++ fd_operate.cpp http_conn.cpp main.cpp -pthread
