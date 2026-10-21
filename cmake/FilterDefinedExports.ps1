param(
    [Parameter(Mandatory = $true)]
    [string]$Objlist,

    [Parameter(Mandatory = $true)]
    [string]$Def
)

$ErrorActionPreference = 'Stop'

$savedLib = $env:LIB
$env:LIB = ''
try {
    Add-Type -Language CSharp -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

public static class CoffExportFilter
{
    private const byte ExternalStorageClass = 2;
    private static readonly Encoding Ascii = Encoding.ASCII;

    public static string Run(string objlistPath, string defPath)
    {
        HashSet<string> wanted = ReadWantedExports(defPath);
        HashSet<string> defined = new HashSet<string>(StringComparer.Ordinal);
        int objectsScanned = 0;

        foreach (string obj in File.ReadLines(objlistPath))
        {
            if (string.IsNullOrWhiteSpace(obj) || !File.Exists(obj))
                continue;

            AddDefinedSymbolsFromObject(obj, wanted, defined, ref objectsScanned);
            if (defined.Count == wanted.Count)
                break;
        }

        List<string> kept = new List<string>();
        int inputCount = 0;
        int removedUndefined = 0;

        foreach (string line in File.ReadLines(defPath))
        {
            ++inputCount;
            string trimmed = line.Trim();
            if (trimmed.Length == 0 || trimmed == "EXPORTS")
            {
                kept.Add(line);
                continue;
            }

            string symbol = FirstToken(trimmed);
            if (defined.Contains(symbol))
                kept.Add(line);
            else
                ++removedUndefined;
        }

        File.WriteAllLines(defPath, kept.ToArray(), new UTF8Encoding(false));
        return string.Format(
            "objects={0}, wanted={1}, defined={2}, input={3}, kept={4}, removed-undefined={5}",
            objectsScanned,
            wanted.Count,
            defined.Count,
            inputCount,
            kept.Count,
            removedUndefined);
    }

    private static HashSet<string> ReadWantedExports(string defPath)
    {
        HashSet<string> wanted = new HashSet<string>(StringComparer.Ordinal);
        foreach (string line in File.ReadLines(defPath))
        {
            string trimmed = line.Trim();
            if (trimmed.Length == 0 || trimmed == "EXPORTS")
                continue;

            wanted.Add(FirstToken(trimmed));
        }
        return wanted;
    }

    private static string FirstToken(string text)
    {
        int end = 0;
        while (end < text.Length && !char.IsWhiteSpace(text[end]))
            ++end;
        return end == text.Length ? text : text.Substring(0, end);
    }

    private static void AddDefinedSymbolsFromObject(
        string path,
        HashSet<string> wanted,
        HashSet<string> defined,
        ref int objectsScanned)
    {
        // One read per object, then the records are parsed in memory: seeking a
        // stream per symbol record discards its buffer, and Engine's objects hold
        // millions of records.
        byte[] data;
        using (FileStream fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
        {
            data = new byte[fs.Length];
            int read = 0;
            while (read < data.Length)
            {
                int chunk = fs.Read(data, read, data.Length - read);
                if (chunk == 0)
                    break;
                read += chunk;
            }
            if (read < data.Length)
                Array.Resize(ref data, read);
        }
        if (data.Length < 20)
            return;

        bool isBigObj = BitConverter.ToUInt16(data, 0) == 0 && BitConverter.ToUInt16(data, 2) == 0xffff;

        int recordSize;
        uint symbolTableOffset;
        uint symbolCount;
        if (isBigObj)
        {
            if (data.Length < 56)
                return;

            recordSize = 20;
            symbolTableOffset = BitConverter.ToUInt32(data, 48);
            symbolCount = BitConverter.ToUInt32(data, 52);
        }
        else
        {
            recordSize = 18;
            symbolTableOffset = BitConverter.ToUInt32(data, 8);
            symbolCount = BitConverter.ToUInt32(data, 12);
        }

        if (symbolTableOffset == 0 || symbolCount == 0)
            return;

        long stringTableOffset = (long)symbolTableOffset + ((long)recordSize * symbolCount);
        if (stringTableOffset + 4 > data.Length)
            return;

        uint stringTableSize = BitConverter.ToUInt32(data, (int)stringTableOffset);
        long stringStart = stringTableOffset + 4;
        long stringLength = 0;
        if (stringTableSize > 4 &&
            stringTableSize <= int.MaxValue &&
            stringTableOffset + stringTableSize <= data.Length)
        {
            stringLength = stringTableSize - 4;
        }

        for (uint i = 0; i < symbolCount;)
        {
            long record = (long)symbolTableOffset + ((long)recordSize * i);
            if (record + recordSize > data.Length)
                break;

            int r = (int)record;
            int sectionNumber = isBigObj ? BitConverter.ToInt32(data, r + 12) : BitConverter.ToInt16(data, r + 12);
            int typeEnd = r + 12 + (isBigObj ? 4 : 2) + 2;
            byte storageClass = data[typeEnd];
            byte auxCount = data[typeEnd + 1];

            if (storageClass == ExternalStorageClass && sectionNumber > 0)
            {
                string name = GetSymbolName(data, r, stringStart, stringLength);
                if (name.Length > 0 && wanted.Contains(name))
                    defined.Add(name);
            }

            i += (uint)(1 + auxCount);
        }

        ++objectsScanned;
    }

    private static string GetSymbolName(byte[] data, int record, long stringStart, long stringLength)
    {
        uint zeroes = BitConverter.ToUInt32(data, record);
        if (zeroes == 0)
        {
            uint offset = BitConverter.ToUInt32(data, record + 4);
            if (offset < 4)
                return string.Empty;

            long start = (long)offset - 4;
            if (start < 0 || start >= stringLength)
                return string.Empty;

            return ReadNullTerminatedAscii(data, (int)(stringStart + start), (int)(stringLength - start));
        }

        return ReadNullTerminatedAscii(data, record, 8);
    }

    private static string ReadNullTerminatedAscii(byte[] bytes, int start, int maxLength)
    {
        if (start < 0 || start >= bytes.Length || maxLength <= 0)
            return string.Empty;

        int limit = Math.Min(bytes.Length, start + maxLength);
        int end = start;
        while (end < limit && bytes[end] != 0)
            ++end;

        return end <= start ? string.Empty : Ascii.GetString(bytes, start, end - start);
    }
}
'@
} finally {
    $env:LIB = $savedLib
}

[CoffExportFilter]::Run($Objlist, $Def)
