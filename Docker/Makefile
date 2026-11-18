# PX4.4 Docker イメージ（Jetson Orin Nano）
#   make            … イメージを作る（px-jetson:latest）
#   sudo で実行しても、UID/GID は元のユーザー（hagi）のものを使う
HOST_USER := $(or $(SUDO_USER),$(USER))
IMAGE     ?= px-jetson:latest

build:
	docker build -t $(IMAGE) \
	  --build-arg USER_UID=$$(id -u $(HOST_USER)) --build-arg USER_GID=$$(id -g $(HOST_USER)) .

.PHONY: build
