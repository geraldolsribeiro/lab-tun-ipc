.DEFAULT_GOAL := up

COMPOSE := docker compose
PC5 := PC_5
PC6 := PC_6
COMM5 := COMM_5

.PHONY: build rebuild up down restart ps logs ping iperf tcpdump clean help

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
	docker exec $(PC5) ping -c 4 -W 2 10.6.0.2

iperf:
	docker exec -d $(PC6) iperf -s
	docker exec $(PC5) iperf -c 10.6.0.2 -b 40m -t 10

tcpdump:
	docker exec $(COMM5) tcpdump -ni any udp port 5000

clean:
	$(COMPOSE) down --rmi local --remove-orphans
	-docker network rm docker_comm_tunnel_lan5 docker_comm_tunnel_lan6 docker_comm_tunnel_backbone

help:
	@echo "Targets: build rebuild up down restart ps logs ping iperf tcpdump clean"
