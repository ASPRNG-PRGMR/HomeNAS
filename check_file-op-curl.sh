TOKEN="Enter your JWT Token"

# 1. mkdir
curl -k -X POST https://nas.local/api/mkdir \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"path":"test_folder"}'
echo ""

# 2. upload
echo "hello nas" > /tmp/testfile.txt
curl -k -X POST "https://nas.local/api/upload?path=test_folder" \
  -H "Authorization: Bearer $TOKEN" \
  -F "file=@/tmp/testfile.txt"
echo ""

# 3. rename (same directory → should log as file.rename)
curl -k -X POST https://nas.local/api/rename \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"from":"test_folder/testfile.txt","to":"test_folder/renamed.txt"}'
echo ""

# 4. second folder, then move into it (different directory → should log as file.move)
curl -k -X POST https://nas.local/api/mkdir \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"path":"test_folder2"}'
echo ""

curl -k -X POST https://nas.local/api/rename \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"from":"test_folder/renamed.txt","to":"test_folder2/renamed.txt"}'
echo ""

# 5. delete
curl -k -X DELETE "https://nas.local/api/file?path=test_folder2/renamed.txt" \
  -H "Authorization: Bearer $TOKEN"
echo ""
