#!/bin/bash

GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'

if ! command -v svgo &> /dev/null; then
    echo -e "${RED}Installing svgo...${NC}"
    npm install -g svgo
fi

cat > /tmp/svgo-temp-config.js << 'EOF'
module.exports = {
  plugins: [
    "preset-default",
    "removeDimensions",
    "sortAttrs",
    "convertStyleToAttrs"
  ]
};
EOF

total=$(find . -type f -name "*.svg" -not -path "./node_modules/*" | wc -l)
count=0

echo -e "${GREEN}Found SVG: $total${NC}"

find . -type f -name "*.svg" -not -path "./node_modules/*" | while read -r file; do
    count=$((count + 1))
    original_size=$(stat -c%s "$file" 2>/dev/null || stat -f%z "$file" 2>/dev/null)
    
    svgo --multipass --config=/tmp/svgo-temp-config.js "$file" -o "$file" 2>/dev/null
    
    new_size=$(stat -c%s "$file" 2>/dev/null || stat -f%z "$file" 2>/dev/null)
    saved=$((original_size - new_size))
    printf "${GREEN}[%3d/$total]${NC} %-40s saved: %6d bytes\n" "$count" "$(basename "$file")" "$saved"
done

rm /tmp/svgo-temp-config.js
echo -e "${GREEN}Done!${NC}"