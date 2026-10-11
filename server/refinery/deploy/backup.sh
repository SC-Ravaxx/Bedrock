# For the stack's daily backup script (it sets BACKUP_DIR, DATE and ERRORS):
# a consistent copy through SQLite's online backup, while the service runs.

# ── SQLite: Bedrock refinery observations ─────────────────
echo "Backing up Bedrock refinery observations..."
if docker exec bedrock-api python app.py --backup /data/backup.db >/dev/null 2>&1 \
    && docker cp bedrock-api:/data/backup.db "$BACKUP_DIR/bedrock-refinery_$DATE.db" 2>/dev/null; then
    SIZE=$(du -h "$BACKUP_DIR/bedrock-refinery_$DATE.db" | cut -f1)
    echo "✓ Bedrock refinery observations backed up ($SIZE)"
else
    ERRORS+=("Bedrock refinery backup FAILED")
    echo "✗ Bedrock refinery backup FAILED"
    rm -f "$BACKUP_DIR/bedrock-refinery_$DATE.db"
fi
