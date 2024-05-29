# Сборка Qt 5.15.14 Cadabra Edition

0. Создать необходимые директории:
```bash
mkdir -p <Qt5.15.14.CE-source-path>
mkdir -p <Qt5.15.14.CE-install-path>
```

1. Склонировать репозиторий с Qt5.15.12 Cadabra Edition:
```bash
git clone https://github.com/cadabra-systems/Qt5.15.14.CE.git <Qt5.15.14.CE-source-path>
```

## Сборка для Android на Linux-based OS

## Сборка для Android на macOS
2a. Установить переменные окружения:
```bash
export JAVA_HOME=/usr/lib/jvm/java-8-openjdk
export PATH=$JAVA_HOME/bin:$PATH
```

2b. Запустить скрипт конфигурации:
```bash
<Qt5.15.14.CE-source-path>/configure -xplatform android-clang \
-prefix <Qt5.15.14.CE-install-path> \
-disable-rpath \
-android-sdk <AndroidSDK-root-path> \
-android-ndk <AndroidSDK-root-path>/ndk/22.1.7171670 \
-android-ndk-host darwin-x86_64
-opengl es2 \
-no-dbus \
-no-qpa-platform-guard \
-sql-sqlite \
-openssl \
-I<OpenSSL1.1.1m-source-path>/include \
-L<OpenSSL1.1.1m-library-path> \
-nomake tests \
-nomake examples \
-verbose \
-opensource \
-confirm-license
```

## Сборка для iOS на macOS
2. Запустить скрипт конфигурации:
```bash
<Qt5.15.14.CE-source-path>/configure -xplatform macx-ios-clang \
-debug-and-release \
-prefix <Qt5.15.14.CE-install-path> \
-sql-sqlite \
-nomake tests \
-nomake examples \
-opensource \
-confirm-license
```
    
3. Запустить сборку:
Для удобства запуска многопоточной сборки определить алиас: `alias nproc="sysctl -n hw.logicalcpu"`
```bash
make --jobs=$(nproc)
sudo make install
```
# Qt5.15.14.CE
