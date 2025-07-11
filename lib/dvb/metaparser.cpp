#include <errno.h>
#include <fstream>
#include <iostream>
#include <lib/base/cfile.h>
#include <lib/base/eerror.h>
#include <lib/dvb/metaparser.h>
#include <lib/service/iservice.h>
#include <sstream>
#include <sys/stat.h>

eDVBMetaParser::eDVBMetaParser() {
	m_time_create = 0;
	m_data_ok = 0;
	m_length = 0;
	m_filesize = 0;
	m_packet_size = 188;
	m_scrambled = 0;
}

static int getctime(const std::string& basename) {
	struct stat s = {};
	if (::stat(basename.c_str(), &s) == 0) {
		return s.st_ctime;
	}
	return 0;
}

static long long fileSize(const std::string& basename) {
	long long filesize = 0;
	char buf[8] = {};
	std::string splitname;
	struct stat64 s = {};

	/* get filesize */
	if (!stat64(basename.c_str(), &s))
		filesize = (long long)s.st_size;
	/* handling for old splitted recordings (enigma 1) */
	int slice = 1;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
	while (true) {
		snprintf(buf, sizeof(buf), ".%03d", slice++);
		splitname = basename + buf;
		if (stat64(splitname.c_str(), &s) < 0)
			break;
		filesize += (long long)s.st_size;
	}
#pragma GCC diagnostic pop
	return filesize;
}

int eDVBMetaParser::parseFile(const std::string& basename) {
	/* first, try parsing the .meta file */
	if (!parseMeta(basename))
		return 0;

	/* otherwise, use recordings.epl */
	if (!parseRecordings(basename))
		return 0;
	m_filesize = fileSize(basename);
	m_time_create = getctime(basename);
	return -1;
}

int eDVBMetaParser::parseMeta(const std::string& tsname) {
	/* if it's a PVR channel, recover service id. */
	std::string filename = tsname + ".meta";
	CFile f(filename.c_str(), "r");

	if (!f)
		return -ENOENT;

	int linecnt = 0;

	m_time_create = 0;

	while (1) {
		char line[4096];
		if (!fgets(line, 4096, f))
			break;
		size_t len = strlen(line);
		if (len && line[len - 1] == '\n') {
			--len;
			line[len] = 0;
		}
		if (len && line[len - 1] == '\r') {
			--len;
			line[len] = 0;
		}

		switch (linecnt) {
			case iDVBMetaFile::idServiceRef:
				m_ref = eServiceReferenceDVB(line);
				break;
			case iDVBMetaFile::idName:
				m_name = line;
				break;
			case iDVBMetaFile::idDescription:
				m_description = line;
				break;
			case iDVBMetaFile::idCreated:
				m_time_create = atoi(line);
				if (m_time_create == 0) {
					m_time_create = getctime(tsname);
				}
				break;
			case iDVBMetaFile::idTags:
				m_tags = line;
				break;
			case iDVBMetaFile::idLength:
				m_length = atoll(line); // movielength in pts
				break;
			case iDVBMetaFile::idFileSize:
				m_filesize = atoll(line);
				break;
			case iDVBMetaFile::idServiceData:
				m_service_data = line;
				break;
			case iDVBMetaFile::idPacketSize:
				m_packet_size = atoi(line);
				if (m_packet_size <= 0) {
					/* invalid value, use default */
					m_packet_size = 188;
				}
				break;
			case iDVBMetaFile::idScrambled:
				m_scrambled = atoi(line);
				break;
			default:
				break;
		}
		++linecnt;
	}
	m_data_ok = 1;
	return 0;
}

int eDVBMetaParser::parseRecordings(const std::string& filename) {
	std::string::size_type slash = filename.rfind('/');
	if (slash == std::string::npos)
		return -1;

	std::string recordings = filename.substr(0, slash) + "/recordings.epl";

	CFile f(recordings.c_str(), "r");
	if (!f) {
		//		eDebug("[eDVBMetaParser] no recordings.epl found: %s: %m", recordings.c_str());
		return -1;
	}

	std::string description;
	eServiceReferenceDVB ref;

	//	eDebug("[eDVBMetaParser] parsing recordings.epl..");

	while (1) {
		char line[1024];
		if (!fgets(line, 1024, f))
			break;

		size_t len = strlen(line);
		if (len < 2)
			// Lines with less than one char aren't meaningful
			continue;
		// Remove trailing \r\n
		--len;
		line[len] = 0;
		if (line[len - 1] == '\r')
			line[len - 1] = 0;

		if (strncmp(line, "#SERVICE: ", 10) == 0)
			ref = eServiceReferenceDVB(line + 10);
		else if (strncmp(line, "#DESCRIPTION: ", 14) == 0)
			description = line + 14;
		else if ((line[0] == '/') &&
				 (ref.path.substr(ref.path.find_last_of('/')) == filename.substr(filename.find_last_of('/')))) {
			//			eDebug("[eDVBMetaParser] hit! ref %s descr %s", m_ref.toString().c_str(), m_name.c_str());
			m_ref = ref;
			m_name = description;
			m_description = "";
			m_time_create = getctime(filename);
			m_length = 0;
			m_filesize = fileSize(filename);
			m_data_ok = 1;
			m_scrambled = 0;
			updateMeta(filename);
			return 0;
		}
	}
	return -1;
}

int eDVBMetaParser::updateMeta(const std::string& tsname) {
	/* write meta file only if we have valid data. Note that we might convert recordings.epl data to .meta, which is
	 * fine. */
	if (!m_data_ok)
		return -1;
	std::string filename = tsname + ".meta";
	eServiceReference ref = m_ref;
	ref.path = "";

	CFile f(filename.c_str(), "w");
	if (!f) {
		eDebug("[eDVBMetaParser] error updateMeta, couldn't open '%s' for writing: %m", filename.c_str());
		return -ENOENT;
	}

	if (ref.getName().empty()) {
		ePtr<iServiceHandler> service_center;
		ePtr<iStaticServiceInformation> service_info;
		eServiceCenter::getInstance(service_center);
		service_center->info(ref, service_info);
		if (service_info) {
			std::string service_name;
			service_info->getName(ref, service_name);
			ref.setName(service_name);
		}
	}
	fprintf(f, "%s\n%s\n%s\n%d\n%s\n%lld\n%lld\n%s\n%d\n%d\n", ref.toString().c_str(), m_name.c_str(),
			m_description.c_str(), m_time_create, m_tags.c_str(), m_length, m_filesize, m_service_data.c_str(),
			m_packet_size, m_scrambled);
	return 0;
}

static bool isStartOfMetadata(const std::string& line) {
	return line.size() > 2 && isupper(line[0]) && line[1] == ' ' && isdigit(line[2]);
}

std::string eDVBMetaParser::parseTxtFile(const std::string& basename) {
	std::string filename = basename;
	filename.erase(filename.length() - 2, 2);
	filename += "txt";
	std::ifstream file(filename);
	std::string result = "";
	if (file.is_open()) {
		std::string line;
		std::ostringstream content;

		while (getline(file, line)) {
			if (isStartOfMetadata(line))
				break;
			content << line << '\n';
		}
		result = content.str();
	}
	return result;
}