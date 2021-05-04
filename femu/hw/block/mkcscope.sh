#!/bin/sh
rm -rf cscope.files cscope.out tags
ctags -R .
#find . \( -name '*.c' -o -name '*.cpp' -o -name '*.cc' -o -name '*.h' -o -name '*.s' -o -name '*.S' \) -print > cscope.files
#cscope -i cscope.files

find ./ -name '*.[cCsShH]' > cscope.files
#find ./ -name '*.[sShH]' > cscope.files
