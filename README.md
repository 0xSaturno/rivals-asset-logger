# Rivals Asset Logger

![Preview](https://i.imgur.com/8plhJIL.png)

an .asi module for marvel rivals to log loaded assets and categorize them. you can double click or right click stuff to see pointers inside it, and dump everything to a csv.

how it works:
it basically just injects an imgui window via d3d11 and runs a background thread that constantly scrapes `GObjects`. it tracks when new assets spawn in memory (like when u load into matches or heroes use abilities). 

it auto-sorts things based on their class names (like blueprints, sounds, skeletal meshes, abilities, etc.) so u can easily filter out the garbage you dont care about. if u click an asset it loosely scans its memory to find other assets it might be referencing.

to build just open the sln in visual studio 2022 and compile for Release x64, or just run:
`msbuild AssetLogger.sln /p:Configuration=Release /p:Platform=x64`
