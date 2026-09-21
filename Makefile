.DEFAULT_GOAL := up

COMPOSE := docker compose
PC5 := PC_5
PC6 := PC_6
CMM5 := CMM_5

.PHONY: build rebuild up down restart ps logs ping ping-rate ping-stress iperf tcpdump clean help

build:
	$(COMPOSE) build

rebuild:
	$(COMPOSE) build --no-cache

up:
	$(COMPOSE) up -d

down:
	$(COMPOSE) down

restart: down up

ps:
	$(COMPOSE) ps

logs:
	$(COMPOSE) logs --tail=100

ping:
	@echo "Running 10 ICMP echo requests; ping reports min/avg/max/mdev RTT statistics"
	docker exec $(PC5) ping -c 10 -W 2 10.6.0.2

ping-rate:
	@echo "Running deterministic 2ms ping without flood mode"
	docker exec $(PC5) ping -i 0.002 -s 11000 -c 1000 -W 2 10.6.0.2

ping-stress:
	@echo "Running aggressive flood ping with 11000-byte payload"
	docker exec $(PC5) ping -f -i 0.002 -s 11000 -c 1000 -W 2 10.6.0.2

iperf:
	docker exec -d $(PC6) iperf -s
	docker exec $(PC5) iperf -c 10.6.0.2 -b 40m -t 10

tcpdump:
	docker exec $(CMM5) tcpdump -ni any udp port 5000

clean:
	$(COMPOSE) down --rmi local --remove-orphans
	-docker network rm docker_cmm_tunnel_lan5 docker_cmm_tunnel_lan6 docker_cmm_tunnel_backbone

help:
	@echo "Targets: build rebuild up down restart ps logs ping ping-rate ping-stress iperf tcpdump clean"
