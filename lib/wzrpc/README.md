# wzrpc

### Goals
Allow remote control of application:
- for automated tests
- for automated fight statistics
- for developing AI/bots in any other language than Javascript; being fully unconstrained by any performance considerations; and to have a nice debugging experience


### Choices made
- Why not more efficient/fast protocol, like Protobuffs? Why not using an RPC lib (https://github.com/rpclib/rpclib)?
Don't want to add dependencies, we already have msgpack coming with nlohmann::json
Also rpclib has a wierd API I don't understand, doesnt seem flexible enough

- Why not CBOR/BSON/UBJSON, they come with nlohmann::json too?
https://nlohmann.github.io/json/features/binary_formats/ states that only MessagePack and UBJSON have total completeness. Out of those 2, it seems like MessagePack has
more widespread use

- There is no `on_frame` event, by the time rpc message arrives, information will be outdated!
Yup, just like for a human I guess

