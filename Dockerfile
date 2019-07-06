FROM debian:buster

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get -y update && apt-get -y upgrade

########## Build and install V8 ##########

# No precompiled binaries :(
# See: https://v8.dev/docs/embed

# Install system dependencies
RUN apt-get -y install curl git python

# Install depot_tools
RUN cd /root && git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git
ENV PATH="$PATH:/root/depot_tools"
RUN gclient

# Check out
# To pick a good stable version, find linux/stable/current here: https://omahaproxy.appspot.com
# Then use the tool to look up V8.
# API changes: https://docs.google.com/document/d/1g8JFi8T_oAE_7uAri7Njtig7fKaPDfotU6huOa1alds
RUN mkdir /root/v8
RUN cd /root/v8 && fetch v8
RUN cd /root/v8/v8 && git checkout tags/7.5.288.23
RUN cd /root/v8/v8 && gclient sync

# Prepare config and build
RUN apt-get -y install libglib2.0-dev
RUN cd /root/v8/v8 && tools/dev/v8gen.py x64.release.sample
RUN cd /root/v8/v8 && ninja -C out.gn/x64.release.sample v8_monolith

# "Install" artifacts
RUN cd /root/v8/v8 && cp -R include/v8*.h include/libplatform /usr/local/include
RUN cd /root/v8/v8 && cp out.gn/x64.release.sample/obj/libv8_*.a /usr/local/lib

########## Eval the Evil ##########

# System dependencies
RUN apt-get -y install clang libboost-system1.67.0 libboost-system1.67-dev nodejs
