add_executable(signingkeyrecoverytest
    signingkeyrecoverytest.cpp
    ../src/signingkeyrecovery.cpp
    ../src/signingkeyrecovery.h
    ../src/securitydiagnostics.cpp
    ../src/securitydiagnostics.h
)
target_include_directories(signingkeyrecoverytest PRIVATE ../src)
target_link_libraries(signingkeyrecoverytest PRIVATE Qt6::Core Qt6::Test)
add_test(NAME signingkeyrecoverytest COMMAND signingkeyrecoverytest)
