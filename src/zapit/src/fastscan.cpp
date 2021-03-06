/*
        Neutrino-GUI  -   DBoxII-Project

        Copyright (C) 2011 CoolStream International Ltd

        License: GPLv2

        This program is free software; you can redistribute it and/or modify
        it under the terms of the GNU General Public License as published by
        the Free Software Foundation;

        This program is distributed in the hope that it will be useful,
        but WITHOUT ANY WARRANTY; without even the implied warranty of
        MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
        GNU General Public License for more details.

        You should have received a copy of the GNU General Public License
        along with this program; if not, write to the Free Software
        Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include <fcntl.h>
#include <unistd.h>

#include <zapit/client/zapitclient.h>
#include <zapit/debug.h>
#include <zapit/settings.h>
#include <zapit/types.h>
#include <zapit/dvbstring.h>
#include <zapit/satconfig.h>
#include <zapit/scan.h>
#include <zapit/zapit.h>
#include <dmx.h>
#include <math.h>
#include <eitd/edvbstring.h>

#define SEC_SIZE 4096
//#define SCAN_DEBUG

fast_scan_operator_t fast_scan_operators [OPERATOR_MAX] = {
	{ CD_OPERATOR_ID,        900, (char *) "CanalDigitaal (SD)" },
	{ CD_OPERATOR_ID,        901, (char *) "CanalDigitaal (HD)" },
	{ TVV_OPERATOR_ID,       910, (char *) "TV Vlaanderen (SD)" },
	{ TVV_OPERATOR_ID,       911, (char *) "TV Vlaanderen (HD)" },
	{ TELESAT_OPERATOR_ID,   920, (char *) "TéléSAT Belgique" },
	{ TELESAT_OPERATOR_ID,   921, (char *) "TéléSAT Luxembourg" },
	{ HDAUSTRIA_OPERATOR_ID, 950, (char *) "HD Austria" },
	{ SKYLINK_OPERATOR_ID,   30,  (char *) "Skylink Czech Republic" },
	{ SKYLINK_OPERATOR_ID,   31,  (char *) "Skylink Slovak Republic" },
	{ HELLO_OPERATOR_ID,     951, (char *) "Hello" }
};

extern CBouquetManager *g_bouquetManager;
extern CBouquetManager* scanBouquetManager;
extern CZapitClient::bouquetMode bouquetMode;
extern transponder_list_t transponders; //  defined in zapit.cpp

typedef struct diseq_test_param {
	int position;
	int frequency;
	int symbol_rate;
	fe_code_rate_t fec_inner;
	int polarization;
} diseq_test_param_t;

diseq_test_param_t diseqc_test[] = {
	// { 192, 12515000, 22000000, FEC_5_6, 0 }, // test TP
	{ 192, 12552000, 22000000, FEC_5_6,         1 },
	{ 235, 12168000, 27500000, FEC_3_4,         1 },
	{ 282, 11778000, 27500000, FEC_2_3,         1 }, // FIXME: no such TP on lyngsat ??
	{ 130, 11681000, 27500000, FEC_S2_8PSK_3_4, 0 },
	{ 90,  12074000, 27500000, FEC_S2_8PSK_3_4, 1 },
};
#define diseqc_test_count (sizeof(diseqc_test)/sizeof(diseq_test_param_t))

bool CServiceScan::TestDiseqcConfig(int num)
{
	if(num >= OPERATOR_MAX) {
		INFO("[fast scan] invalid operator %d", num);
		return false;
	}
	fast_scan_operator_t *op = &fast_scan_operators[num];
	printf("[fast scan] testing diseqc config for operator %d [%s], pid 0x%x\n", op->id, op->name, op->pid);
	InitFastscanLnb(op->id);

	std::set<int> scanned;
	satellite_map_t &satmap = frontend->getSatellites();
	for (int diseqc = 0; diseqc < 4; diseqc++) {
		for (unsigned i = 0; i < diseqc_test_count; i++) {
			if ((op->id == HELLO_OPERATOR_ID) && (diseqc_test[i].position == 282))
				continue;
			if ((op->id != HELLO_OPERATOR_ID) && (diseqc_test[i].position == 90))
				continue;

			std::set<int>::iterator it = scanned.find(diseqc_test[i].position);
			if (it != scanned.end())
				continue;

			FrontendParameters feparams;
			feparams.dvb_feparams.frequency = diseqc_test[i].frequency;
			feparams.dvb_feparams.u.qpsk.symbol_rate = diseqc_test[i].symbol_rate;
			feparams.dvb_feparams.u.qpsk.fec_inner = diseqc_test[i].fec_inner;

			int old_diseqc = satmap[diseqc_test[i].position].diseqc;
			satmap[diseqc_test[i].position].diseqc = diseqc;

			if (tuneFrequency(&feparams, diseqc_test[i].polarization, diseqc_test[i].position)) {
				scanned.insert(diseqc_test[i].position);
				break;
			}
			satmap[diseqc_test[i].position].diseqc = old_diseqc;
		}
	}
	return true;
}

void CServiceScan::InitFastscanLnb(int id)
{
	CServiceManager::getInstance()->InitSatPosition(192, NULL, true);
	CServiceManager::getInstance()->InitSatPosition(235, NULL, true);
	CServiceManager::getInstance()->InitSatPosition(130, NULL, true);
	if (id == HELLO_OPERATOR_ID)
		CServiceManager::getInstance()->InitSatPosition(90, NULL, true);
	else
		CServiceManager::getInstance()->InitSatPosition(282, NULL, true);

	satellite_map_t & satmap = CServiceManager::getInstance()->SatelliteList();

	frontend = CFEManager::getInstance()->getFE(0);

	satmap[130].configured = 1;
	satmap[192].configured = 1;
	satmap[235].configured = 1;
	if (id == HELLO_OPERATOR_ID)
		satmap[90].configured = 1;
	else
		satmap[282].configured = 1;

	frontend->setSatellites(satmap);
	frontend->setDiseqcType(DISEQC_1_0);
	CFEManager::getInstance()->saveSettings();
	SetFrontend(192);
}

void CServiceScan::ReportFastScan(FrontendParameters &feparams, uint8_t polarization, t_satellite_position satellitePosition)
{
	if (quiet_fastscan)
		return;

	std::string satname = CServiceManager::getInstance()->GetSatelliteName(satellitePosition);
	uint32_t actual_polarisation = ((feparams.dvb_feparams.u.qpsk.symbol_rate/1000) << 16)
		| (feparams.dvb_feparams.u.qpsk.fec_inner << 8) | (uint)polarization;
	uint32_t actual_freq = feparams.dvb_feparams.frequency;

	CZapit::getInstance()->SendEvent(CZapitClient::EVT_SCAN_SATELLITE, satname.c_str(), satname.size() + 1);
	CZapit::getInstance()->SendEvent(CZapitClient::EVT_SCAN_REPORT_FREQUENCYP, &actual_polarisation,sizeof(actual_polarisation));
	CZapit::getInstance()->SendEvent(CZapitClient::EVT_SCAN_REPORT_FREQUENCY, &actual_freq,sizeof(actual_freq));
}

bool CServiceScan::FastscanTune(int id)
{
	uint8_t polarization;
	FrontendParameters feparams;
	bool res = false;

	feparams.dvb_feparams.frequency = 12070000;
	feparams.dvb_feparams.u.qpsk.symbol_rate = 27500000;
	feparams.dvb_feparams.u.qpsk.fec_inner = FEC_3_4;
	polarization = 0;

	ReportFastScan(feparams, polarization, 235);
	if(!tuneFrequency(&feparams, polarization, 235)) {
		printf("[fast scan] tune failed, try backup\n");
		if (id == HELLO_OPERATOR_ID) {
			feparams.dvb_feparams.frequency = 12074000;
			feparams.dvb_feparams.u.qpsk.symbol_rate = 27500000;
			feparams.dvb_feparams.u.qpsk.fec_inner = FEC_3_4;
			polarization = 1;
			ReportFastScan(feparams, polarization, 90);
			res = tuneFrequency(&feparams, polarization, 90);
		} else {
			feparams.dvb_feparams.frequency = 12515000;
			feparams.dvb_feparams.u.qpsk.symbol_rate = 22000000;
			feparams.dvb_feparams.u.qpsk.fec_inner = FEC_5_6;
			ReportFastScan(feparams, polarization, 192);
			res = tuneFrequency(&feparams, polarization, 192);
		}
	} else
		res = true;

	return res;
}

bool CServiceScan::ScanFast()
{
	fast_scan_type_t * fast_type = (fast_scan_type_t *) scan_arg;
	fs_operator_t num = fast_type->op;
	if(num >= OPERATOR_MAX) {
		INFO("[fast scan] invalid operator %d", num);
		return false;
	}
	return ScanFast(num);
}

bool CServiceScan::ScanFast(int num, bool reload)
{
	fast_scan_operator_t *op;
	if(num >= OPERATOR_MAX) {
		INFO("[fast scan] invalid operator %d", num);
		goto _err;
	}
	op = &fast_scan_operators[num];

	found_transponders = 0;
	found_channels = 0;
	found_tv_chans = 0;
	found_data_chans = 0;
	found_radio_chans = 0;

	printf("[fast scan] scaning operator %d [%s], pid 0x%x\n", op->id, op->name, op->pid);

	if (!FastscanTune(op->id))
		goto _err;

	if (!quiet_fastscan)
		CZapit::getInstance()->SendEvent(CZapitClient::EVT_SCAN_SATELLITE, op->name, strlen(op->name)+1);

	scanBouquetManager = new CBouquetManager();

	if(ParseFnt(op->pid, op->id))
		ParseFst(op->pid, op);

	//FIXME move to Cleanup() ?
	if(found_channels) {
		CZapitClient myZapitClient;
		CServiceManager::getInstance()->SaveServices(true);
		scanBouquetManager->saveBouquets(bouquetMode, "");
		g_bouquetManager->saveBouquets();
		g_bouquetManager->saveUBouquets();
		//g_bouquetManager->clearAll();
		if (reload)
			g_bouquetManager->loadBouquets();
		Cleanup(true);
		myZapitClient.reloadCurrentServices();
	} else {
		Cleanup(false);
		frontend->setTsidOnid(0);
		//zapit(live_channel_id, 0);
	}
	fast_services_sat.clear();
	fast_services_freq.clear();
	fast_services_number.clear();

	printf("[fast scan] fast scan done, found %d transponders and %d services\n", found_transponders, found_channels);
	return true;
_err:
	Cleanup(false);
	return false;
}

bool CServiceScan::ReadFstVersion(int num)
{
	if(num >= OPERATOR_MAX) {
		printf("[fast scan] invalid operator %d\n", num);
		return false;
	}
	fast_scan_operator_t *op = &fast_scan_operators[num];
	unsigned short operator_id = op->id;
	unsigned short pid = op->pid;

	printf("[FST VERSION] scaning operator %d [%s], pid 0x%x\n", op->id, op->name, op->pid);
	if (!FastscanTune(op->id))
		return false;

	frontend->setTsidOnid(0);

	cDemux * dmx = new cDemux();
	dmx->Open(DMX_PSI_CHANNEL);
	unsigned char buffer[SEC_SIZE];

	unsigned char filter[DMX_FILTER_SIZE];
	unsigned char mask[DMX_FILTER_SIZE];

	memset(filter, 0x00, DMX_FILTER_SIZE);
	memset(mask, 0x00, DMX_FILTER_SIZE);

	filter[0] = 0xBD;
	filter[1] = (operator_id >> 8) & 0xff;
	filter[2] = operator_id & 0xff;
	mask[0] = mask[1] = mask[2] = 0xFF;

	if (dmx->sectionFilter(pid, filter, mask, 3) < 0) {
		delete dmx;
		return false;
	}
	if (dmx->Read(buffer, SEC_SIZE) < 0) {
		delete dmx;
		return false;
	}

	fst_version = (buffer[5] >> 1) & 0x1f;
	printf("[FST VERSION] version %02x\n", fst_version);
	delete dmx;
	return true;
}

bool CServiceScan::ParseFst(unsigned short pid, fast_scan_operator_t * op)
{
	int secdone[255];
	int sectotal = -1;
	unsigned short operator_id = op->id;
	CZapitBouquet* bouquet;
	int bouquetId;

	memset(secdone, 0, 255);

	cDemux * dmx = new cDemux();
	dmx->Open(DMX_PSI_CHANNEL);

	unsigned char buffer[SEC_SIZE];

	/* position in buffer */
	unsigned short pos;
	unsigned short pos2;

	/* service_description_section elements */
	unsigned short section_length;
	unsigned short transport_stream_id = 0;
	unsigned short original_network_id = 0;
	unsigned short operator_network_id = 0;

	unsigned short service_id;
	unsigned short descriptors_loop_length;
	unsigned short video_pid, audio_pid, pcr_pid;

	unsigned char filter[DMX_FILTER_SIZE];
	unsigned char mask[DMX_FILTER_SIZE];

	memset(filter, 0x00, DMX_FILTER_SIZE);
	memset(mask, 0x00, DMX_FILTER_SIZE);

	filter[0] = 0xBD;
	filter[1] = (operator_id >> 8) & 0xff;
	filter[2] = operator_id & 0xff;
	mask[0] = mask[1] = mask[2] = 0xFF;

	printf("[FST] scaning pid %d operator %d\n", pid, operator_id);

	if (dmx->sectionFilter(pid, filter, mask, 3) < 0) {
		delete dmx;
		return false;
	}
#if 0
	g_bouquetManager->clearAll();
	CServiceManager::getInstance()->RemoveAllChannels();
#endif
	do {
		if (dmx->Read(buffer, SEC_SIZE) < 0) {
			delete dmx;
			return false;
		}
		if(buffer[0] != 0xBD)
		        printf("[FST] ******************************************* Bogus section received: 0x%x\n", buffer[0]);


		section_length = ((buffer[1] & 0x0F) << 8) | buffer[2];
		operator_network_id = (buffer[3] << 8) | buffer[4];

		fst_version = (buffer[5] >> 1) & 0x1f;
		unsigned char secnum = buffer[6];
		printf("[FST] version %x section %X last %X operator 0x%x -> %s\n", fst_version, buffer[6], buffer[7], operator_network_id, secdone[secnum] ? "skip" : "use");

		if(secdone[secnum])
			continue;

		secdone[secnum] = 1;
		sectotal++;

		for (pos = 8; pos < section_length - 1; pos += descriptors_loop_length + 18) {
			original_network_id = (buffer[pos] << 8) | buffer[pos+1];
			transport_stream_id = (buffer[pos+2] << 8) | buffer[pos+3];
			service_id = (buffer[pos+4] << 8) | buffer[pos+5];
			video_pid = (buffer[pos+6] << 8) | buffer[pos+7];
			audio_pid = (buffer[pos+8] << 8) | buffer[pos+9];
			pcr_pid = (buffer[pos+14] << 8) | buffer[pos+15];

			//printf("[FST] onid %x tid %x sid %x vpid %x apid %x pcr %x\n", original_network_id, transport_stream_id, service_id, video_pid, audio_pid, pcr_pid);
			descriptors_loop_length = ((buffer[pos + 16] & 0x0F) << 8) | buffer[pos + 17];

			for (pos2 = pos + 18; pos2 < pos + descriptors_loop_length + 18; pos2 += buffer[pos2 + 1] + 2) {
				switch (buffer[pos2]) {
				case 0x48:
					{
						unsigned char * dbuf = &buffer[pos2];
						uint8_t service_type = dbuf[2];
						uint8_t service_provider_name_length = dbuf[3];
						/*uint8_t service_name_length = dbuf[service_provider_name_length + 4];*/
						int service_name_length = (2 + dbuf[1]) - (4 + service_provider_name_length + 1);

						t_satellite_position satellitePosition = 0;
						freq_id_t freq = 0;
						int num = 0;

						t_channel_id channel_id = CREATE_CHANNEL_ID64;

						std::map <t_channel_id, t_satellite_position>::iterator sIt = fast_services_sat.find(channel_id);
						if(sIt != fast_services_sat.end()) {
							satellitePosition = sIt->second;
#if 0
							sat_iterator_t sit = satellitePositions.find(satellitePosition);
							if(sit != satellitePositions.end())
								sit->second.have_channels = true;
#endif
						}

						std::map <t_channel_id, freq_id_t>::iterator fIt = fast_services_freq.find(channel_id);
						if(fIt != fast_services_freq.end())
							freq = fIt->second;

						std::map <t_channel_id, int>::iterator nIt = fast_services_number.find(channel_id);
						if(nIt != fast_services_number.end())
							num = nIt->second;

						std::string providerName = convertDVBUTF8((const char*)&(dbuf[4]), service_provider_name_length, 1, 1);
						std::string serviceName  = convertDVBUTF8((const char*)&(dbuf[4 + service_provider_name_length + 1]), service_name_length, 1, 1);

#ifdef SCAN_DEBUG
						printf("[FST] #%04d at %04d: net %04x tp %04x sid %04x v %04x a %04x pcr %04x frq %05d type %d prov [%s] name [%s]\n", num, satellitePosition, original_network_id, transport_stream_id, service_id, video_pid, audio_pid, pcr_pid, freq, service_type, providerName.c_str(), serviceName.c_str());
#endif
						if (!quiet_fastscan)
							ChannelFound(service_type, providerName, serviceName);
						else
							found_channels++;

						channel_id = CREATE_CHANNEL_ID64;

						CZapitChannel * newchannel;

						int flist[5] = { freq, freq-1, freq+1, freq-2, freq+2 };
						for(int i = 0; i < 5; i++) {
							freq_id_t freq_id = flist[i];
							t_channel_id newid = CZapitChannel::makeChannelId(satellitePosition,
									freq_id, transport_stream_id, original_network_id, service_id);
							newchannel = CServiceManager::getInstance()->FindChannel(newid);
							if(newchannel)
								break;
						}
						if(newchannel == NULL) {
							newchannel = new CZapitChannel (
									serviceName,
									channel_id,
									service_type,
									satellitePosition,
									freq);
							newchannel->deltype = FE_QPSK;
							CServiceManager::getInstance()->AddChannel(newchannel);
						}
						// FIXME detect new/removed
						newchannel->flags = CZapitChannel::UPDATED;
						newchannel->setName(serviceName);
						newchannel->setServiceType(service_type);
						newchannel->setVideoPid(video_pid);
						newchannel->setAudioPid(audio_pid);
						newchannel->setPcrPid(pcr_pid);
						newchannel->setPidsFlag();
						newchannel->number = num;

						char pname[100];
						if (frontend->getInfo()->type == FE_QPSK)
							snprintf(pname, 100, "[%c%03d.%d] %s", satellitePosition > 0? 'E' : 'W', abs(satellitePosition)/10, abs(satellitePosition)%10, providerName.c_str());
						else
							snprintf(pname, 100, "%s", providerName.c_str());

						bouquetId = scanBouquetManager->existsBouquet(pname);

						if (bouquetId == -1) {
							bouquet = scanBouquetManager->addBouquet(std::string(pname), false);
						}
						else
							bouquet = scanBouquetManager->Bouquets[bouquetId];
						bouquet->addService(newchannel);

						if(!quiet_fastscan && newchannel->getServiceType() == 1)
							CZapit::getInstance()->SetCurrentChannelID(newchannel->getChannelID());

						bouquetId = g_bouquetManager->existsUBouquet(op->name);
						if (bouquetId == -1) {
							bouquet = g_bouquetManager->addBouquet(std::string(op->name), true);
							bouquetId = g_bouquetManager->existsUBouquet(op->name);
						}
						else
							bouquet = g_bouquetManager->Bouquets[bouquetId];

						if (!(g_bouquetManager->existsChannelInBouquet(bouquetId, newchannel->getChannelID())))
							bouquet->addService(newchannel);

						if (!quiet_fastscan && num == 1)
							CZapit::getInstance()->SetCurrentChannelID(newchannel->getChannelID());
					}
					break;
				default:
					printf("[FST] descriptor_tag: %02x\n", buffer[pos2]);
					break;
				}
			}
		}
	} while(sectotal < buffer[7]);
	delete dmx;

	bouquetId = g_bouquetManager->existsUBouquet(op->name);
	if (bouquetId >= 0) 
		g_bouquetManager->Bouquets[bouquetId]->sortBouquetByNumber();

	printf("[FST] done\n\n");
	return true;
}

bool CServiceScan::ParseFnt(unsigned short pid, unsigned short operator_id)
{
	int secdone[255];
	int sectotal = -1;

	for(int i = 0; i < 255; i++)
		secdone[i] = 0;

	cDemux * dmx = new cDemux();;
	dmx->Open(DMX_PSI_CHANNEL);

	unsigned char buffer[SEC_SIZE];

	/* position in buffer */
	unsigned short pos;
	unsigned short pos2;

	/* network_information_section elements */
	unsigned short section_length;
	unsigned short network_descriptors_length;
	unsigned short transport_descriptors_length;
	unsigned short transport_stream_loop_length;
	t_transport_stream_id transport_stream_id;
	t_original_network_id original_network_id;
	unsigned short network_id;

	unsigned char filter[DMX_FILTER_SIZE];
	unsigned char mask[DMX_FILTER_SIZE];

	memset(filter, 0x00, DMX_FILTER_SIZE);
	memset(mask, 0x00, DMX_FILTER_SIZE);

	filter[0] = 0xBC;
	filter[1] = (operator_id >> 8) & 0xff;
	filter[2] = operator_id & 0xff;

	mask[0] = mask[1] = mask[2] = 0xFF;

	printf("[FNT] scaning pid %d operator %d\n", pid, operator_id);
	if (dmx->sectionFilter(pid, filter, mask, 3) < 0) {
		delete dmx;
		return false;
	}
	do {
		if (dmx->Read(buffer, SEC_SIZE) < 0) {
			delete dmx;
			return false;
		}
		if(buffer[0] != 0xBC)
			printf("[FNT] ******************************************* Bogus section received: 0x%x\n", buffer[0]);

		section_length = ((buffer[1] & 0x0F) << 8) + buffer[2];
		network_id = ((buffer[3] << 8)| buffer [4]);
		network_descriptors_length = ((buffer[8] & 0x0F) << 8) | buffer[9];

		unsigned char secnum = buffer[6];
		printf("[FNT] section %X last %X network_id 0x%x -> %s\n", secnum, buffer[7], network_id, secdone[secnum] ? "skip" : "use");
		if(secdone[secnum]) // mark sec XX done
			continue;
		secdone[secnum] = 1;
		sectotal++;

		for (pos = 10; pos < network_descriptors_length + 10; pos += buffer[pos + 1] + 2)
		{
			switch (buffer[pos])
			{
				case 0x40:
#ifdef SCAN_DEBUG
					{
						unsigned char len = buffer[pos+1];
						char name[255];
						int i;

						for(i = 0; i < len; i++)
							name[i] = buffer[pos+2+i];
						name[i] = 0;
						printf("[FNT] network name: %s (%d)\n", name, len);

						//printf("[FNT]  network name descriptor\n");
						//network_name_descriptor(buffer + pos);
					}
#endif
					break;
				default:
					printf("[FNT] descriptor_tag: %02x\n", buffer[pos]);
					break;
			}
		}

		transport_stream_loop_length = ((buffer[pos] & 0x0F) << 8) | buffer[pos + 1];

		if (!transport_stream_loop_length)
			continue;

		for (pos += 2; pos < section_length - 3; pos += transport_descriptors_length + 6)
		{
			transport_stream_id = (buffer[pos] << 8) | buffer[pos + 1];
			original_network_id = (buffer[pos + 2] << 8) | buffer[pos + 3];
			transport_descriptors_length = ((buffer[pos + 4] & 0x0F) << 8) | buffer[pos + 5];
			FrontendParameters feparams;
			uint8_t polarization;
			t_satellite_position satellitePosition = 0;
			freq_id_t freq = 0;

			for (pos2 = pos + 6; pos2 < pos + transport_descriptors_length + 6; pos2 += buffer[pos2 + 1] + 2)
			{
				//printf("[FNT] second_descriptor_tag: %02x\n", buffer[pos2]);
				switch (buffer[pos2])
				{
					case 0x43:
						{
							transponder_id_t TsidOnid;
							stiterator stI;

							process_satellite_delivery_system_descriptor(buffer + pos2, &feparams, &polarization, &satellitePosition);
							freq = feparams.dvb_feparams.frequency / 1000;
							TsidOnid = CREATE_TRANSPONDER_ID64(freq, satellitePosition, original_network_id, transport_stream_id);
							stI = transponders.find(TsidOnid);
							if(stI == transponders.end()) {
								transponder t(frontendType, TsidOnid, feparams, polarization);
								transponders.insert(transponder_pair_t(TsidOnid, t));
							}
							found_transponders++;
#if 0
							uint32_t  actual_freq = freq;
							CZapit::getInstance()->SendEvent(CZapitClient::EVT_SCAN_REPORT_FREQUENCY,
									&actual_freq, sizeof(actual_freq));
#endif
							//satellite_delivery_system_descriptor(buffer + pos2, transport_stream_id, original_network_id, satellitePosition, freq);
						}
						break;
					case 0x41:
						process_service_list_descriptor(buffer + pos2, transport_stream_id, original_network_id, 0, 0);
						break;
					case 0x83: // logical_service_desciptor
						process_logical_service_descriptor(buffer + pos2, transport_stream_id, original_network_id, 0, 0);
						break;
					case 0x44:
						//cable_delivery_system_descriptor(buffer + pos2, transport_stream_id, original_network_id, satellitePosition, freq);
						break;
					default:
						printf("[FNT] second_descriptor_tag: %02x\n", buffer[pos2]);
						break;
				}
			}
			std::map <t_channel_id, t_satellite_position>::iterator sIt = fast_services_sat.begin();
			for(; sIt != fast_services_sat.end(); ++sIt)
				if(sIt->second == 0)
					sIt->second = satellitePosition;

			std::map <t_channel_id, freq_id_t>::iterator fIt = fast_services_freq.begin();
			for(; fIt != fast_services_freq.end(); ++fIt)
				if(fIt->second == 0)
					fIt->second = freq;
		}
		if (!quiet_fastscan)
			CZapit::getInstance()->SendEvent(CZapitClient::EVT_SCAN_NUM_TRANSPONDERS,
					&found_transponders, sizeof(found_transponders));
	} while(sectotal < buffer[7]);

	dmx->Stop();
	delete dmx;
	printf("[FNT] done\n\n");
	return true;
}

void CServiceScan::process_logical_service_descriptor(const unsigned char * const buffer, const t_transport_stream_id transport_stream_id, const t_original_network_id original_network_id, t_satellite_position satellitePosition, freq_id_t freq)
{
	for (int i = 0; i < buffer[1]; i += 4) {
		t_service_id service_id = buffer[i + 2] << 8 | buffer[i + 3];
		t_channel_id channel_id = CREATE_CHANNEL_ID64;
		int num = (buffer[i + 4] & 0x3F) << 8 | buffer[i + 5];
		fast_services_number[channel_id] = num;
	}
}

void CServiceScan::process_service_list_descriptor(const unsigned char * const buffer, const t_transport_stream_id transport_stream_id, const t_original_network_id original_network_id, t_satellite_position satellitePosition, freq_id_t freq)
{
	for (int i = 0; i < buffer[1]; i += 3) {
		t_service_id service_id = buffer[i + 2] << 8 | buffer[i + 3];
		t_channel_id channel_id = CREATE_CHANNEL_ID64;
#ifdef SCAN_DEBUG
		std::map <t_channel_id, t_satellite_position>::iterator sIt = fast_services_sat.find(channel_id);
		if(sIt != fast_services_sat.end())
			printf("[FNT] dup channel %llx\n", channel_id);
#endif
		fast_services_sat[channel_id] = satellitePosition;
		fast_services_freq[channel_id] = freq;
	}
}

void CServiceScan::process_satellite_delivery_system_descriptor(const unsigned char * const buffer, FrontendParameters * feparams, uint8_t * polarization, t_satellite_position * satellitePosition)
{
        stiterator tI;
        int modulationSystem, modulationType, /*rollOff,*/ fec_inner;

        feparams->dvb_feparams.frequency = (
                 ((buffer[2] >> 4)      * 100000000) +
                 ((buffer[2] & 0x0F)    * 10000000) +
                 ((buffer[3] >> 4)      * 1000000) +
                 ((buffer[3] & 0x0F)    * 100000) +
                 ((buffer[4] >> 4)      * 10000) +
                 ((buffer[4] & 0x0F)    * 1000) +
                 ((buffer[5] >> 4)      * 100) +
                 ((buffer[5] & 0x0F)    * 10)
		 );

	* satellitePosition = (
                 ((buffer[6] >> 4)      * 1000) +
                 ((buffer[6] & 0x0F)    * 100) +
                 ((buffer[7] >> 4)      * 10) +
                 ((buffer[7] & 0x0F)    * 1)
	);
        feparams->dvb_feparams.inversion = INVERSION_AUTO;

        //rollOff = (buffer[8] >> 4) & 0x03; //alpha_0_35, alpha_0_25, alpha_0_20, alpha_auto
        modulationSystem = (buffer[8] >> 2) & 0x01; // 1= DVB_S2
        modulationType = (buffer[8]) & 0x03; // 1=QPSK, 2=M8PSK

        feparams->dvb_feparams.u.qpsk.symbol_rate = (
                 ((buffer[9] >> 4)      * 100000000) +
                 ((buffer[9] & 0x0F)    * 10000000) +
                 ((buffer[10] >> 4)     * 1000000) +
                 ((buffer[10] & 0x0F)   * 100000) +
                 ((buffer[11] >> 4)     * 10000) +
                 ((buffer[11] & 0x0F)   * 1000) +
                 ((buffer[12] >> 4)     * 100)
                );

        fec_inner = CFrontend::getCodeRate(buffer[12] & 0x0F, modulationSystem);
        if(modulationType == 2)
                fec_inner += 9;

        feparams->dvb_feparams.u.qpsk.fec_inner = (fe_code_rate_t) fec_inner;

        * polarization = (buffer[8] >> 5) & 0x03;

        /* workarounds for braindead broadcasters (e.g. on Telstar 12 at 15.0W) */
        if (feparams->dvb_feparams.frequency >= 100000000)
                feparams->dvb_feparams.frequency /= 10;
        if (feparams->dvb_feparams.u.qpsk.symbol_rate >= 50000000)
                feparams->dvb_feparams.u.qpsk.symbol_rate /= 10;

        feparams->dvb_feparams.frequency = 1000 * ((feparams->dvb_feparams.frequency + 500) / 1000);

#ifdef SCAN_DEBUG
	printf("[FNT] new TP: sat %d freq %d SR %d fec %d pol %d\n", *satellitePosition, feparams->dvb_feparams.frequency, feparams->dvb_feparams.u.qpsk.symbol_rate, fec_inner, * polarization);
#endif
}
