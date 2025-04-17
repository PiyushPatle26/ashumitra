cmake_minimum_required(VERSION 3.5)

# Embed the frontend files
idf_component_register(SRCS ""
                      INCLUDE_DIRS "."
                      EMBED_FILES "index.html" "script.js") 