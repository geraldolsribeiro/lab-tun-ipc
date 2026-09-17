FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    python3 iproute2 iputils-ping iperf net-tools procps tcpdump \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /opt/lab
COPY . /opt/lab/
RUN chmod +x /opt/lab/*.sh /opt/lab/*.py
