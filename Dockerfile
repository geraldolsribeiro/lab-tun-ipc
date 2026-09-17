FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    python3 g++ build-essential iproute2 iputils-ping iperf net-tools procps tcpdump \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /opt/lab
COPY . /opt/lab/
RUN chmod +x /opt/lab/*.sh /opt/lab/*.py && g++ -O3 -std=c++20 -Wall -Wextra -o /opt/lab/apps_cpp /opt/lab/apps.cpp
