g++ -std=c++17 -O2 -pthread \
  bmw_mqtt_bridge.cpp -o bmw_mqtt_bridge \
  $(pkg-config --cflags --libs libmosquitto) -lcurl
