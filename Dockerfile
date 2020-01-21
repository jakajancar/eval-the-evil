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
RUN cd /root/v8/v8 && git checkout tags/7.9.317.33
RUN cd /root/v8/v8 && gclient sync

# Prepare config and build
RUN apt-get -y install libglib2.0-dev
#RUN cd /root/v8/v8 && tools/dev/v8gen.py x64.release.sample # we use custom below, to disable ICU
RUN cd /root/v8/v8 && gn gen out.gn/x64.eval-the-evil --args='\
    is_component_build=false \
    is_debug=false \
    target_cpu="x64" \
    use_custom_libcxx=false \
    v8_monolithic=true \
    v8_use_external_startup_data=false \
    v8_enable_i18n_support=false'
RUN cd /root/v8/v8 && ninja -C out.gn/x64.eval-the-evil v8_monolith

# "Install" artifacts
RUN cd /root/v8/v8 && cp -R include/v8*.h include/libplatform /usr/local/include
RUN cd /root/v8/v8 && cp out.gn/x64.eval-the-evil/obj/libv8_*.a /usr/local/lib

########## Eval the Evil ##########

# Build & test dependencies
RUN apt-get -y install \
    clang \
    libboost-program-options1.67.0 \
    libboost-program-options1.67-dev \
    libboost-system1.67.0 \
    libboost-system1.67-dev \
    libboost-stacktrace1.67.0 \
    libboost-stacktrace1.67-dev \
    make \
    nodejs

# libbacktrace for stack traces, used indirectly by boost-stacktrace
RUN cd /root && \
    git clone https://github.com/ianlancetaylor/libbacktrace.git && \
    cd libbacktrace && \
    git checkout 5a99ff7fed66b8ea8f09c9805c138524a7035ece && \
    ./configure && make && make install


# ghr is used for uploading release artifacts
RUN cd /tmp && \
    curl -fsSLO https://github.com/tcnksm/ghr/releases/download/v0.12.1/ghr_v0.12.1_linux_amd64.tar.gz && \
    tar xf ghr_*.tar.gz && \
    mv ghr_*/ghr /usr/local/bin/ && \
    rm -rf ghr_*

# Development tools
RUN apt-get -y install netcat-openbsd procps
