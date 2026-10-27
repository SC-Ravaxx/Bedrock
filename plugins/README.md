# Plugins

One folder per plugin, as Bedrock keeps it in its data folder (`plugins\<id>`):

    <id>/plugin.json    {"id", "name", "version", "description", "bedrock": oldest Bedrock or ""}
    <id>/main.lua
    <id>/data/*.json    bedrock.data(name)

Text files only (.lua, .json, .txt). The API is in `include/Plugins.h`.

## Test

Developer build (BEDROCK_DEV_TOOLS=ON), runs unsigned plugins:

    Bedrock.exe --plugin-check <plugin folder> <game folder> [state folder]

## Publish

    python tools/release.py <Bedrock.exe>

Signs with the "Bedrock plugin key" in 1Password's SSH agent
(`tools/bedrock-plugin-key.pub`) and writes `dist/`: one `.bplugin` per plugin
and `index.json`. `dist/` goes to `bedrock/plugins/` on the server.

Wikelo's data: `tools/gen_wikelo_from_p4k.py`.
