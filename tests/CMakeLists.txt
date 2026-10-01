add_executable(signingkeyrecoverytest
    signingkeyrecoverytest.cpp
    ../src/signingkeyrecovery.cpp
    ../src/signingkeyrecovery.h
    ../src/securitydiagnostics.cpp
    ../src/securitydiagnostics.h
)
target_include_directories(signingkeyrecoverytest PRIVATE ../src)
target_link_libraries(signingkeyrecoverytest PRIVATE Qt6::Core Qt6::Test flu_bridge)
add_test(NAME signingkeyrecoverytest COMMAND signingkeyrecoverytest)

add_executable(uistatetest uistatetest.cpp ../src/signingkeyrecovery.cpp)
target_include_directories(uistatetest PRIVATE ../src)
target_link_libraries(uistatetest PRIVATE Qt6::Core Qt6::Test flu_bridge)
add_test(NAME uistatetest COMMAND uistatetest)

add_executable(wheelmomentumtest wheelmomentumtest.cpp)
target_link_libraries(wheelmomentumtest PRIVATE Qt6::Core Qt6::Qml Qt6::Test)
target_compile_definitions(wheelmomentumtest PRIVATE
    FLU_QML_PATH=\"${CMAKE_SOURCE_DIR}/src/ui/main.qml\")
add_test(NAME wheelmomentumtest COMMAND wheelmomentumtest)
