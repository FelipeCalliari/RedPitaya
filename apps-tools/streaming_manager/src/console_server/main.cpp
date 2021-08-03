/**
 * $Id: $
 *
 * @brief Red Pitaya streaming server implementation
 *
 * @Author Red Pitaya
 *
 * (c) Red Pitaya  http://www.redpitaya.com
 *
 * This part of code is written in C programming language.
 * Please visit http://en.wikipedia.org/wiki/C_(programming_language)
 * for more details on the language used herein.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include <sys/stat.h>
#include <netinet/in.h>
#include <sys/prctl.h>
#include <errno.h>
#include <arpa/inet.h>
#include <signal.h>
#include <unistd.h>
#include <regex>

#include "rp.h"
#include "StreamingApplication.h"
#include "StreamingManager.h"
#include "ServerNetConfigManager.h"
#include "options.h"

#ifdef Z20_250_12
#include "rp-spi.h"
#include "rp-i2c-max7311.h"
#endif




std::mutex g_print_mtx;
// std::condition_variable cv;
COscilloscope::Ptr osc = nullptr;
CStreamingManager::Ptr s_manger = nullptr;
std::shared_ptr<CStreamingApplication> s_app = nullptr;
std::shared_ptr<ServerNetConfigManager> con_server = nullptr;
std::atomic_bool g_run(true);

const char  *g_argv0 = nullptr;

void StopNonBlocking(int x);

float calibFullScaleToVoltage(uint32_t fullScaleGain) {
    /* no scale */
    if (fullScaleGain == 0) {
        return 1;
    }
    return (float) ((float)fullScaleGain  * 100.0 / ((uint64_t)1<<32));
}

static void handleCloseChildEvents()
{
    struct sigaction sigchld_action; 
    sigchld_action.sa_handler = SIG_DFL,
    sigchld_action.sa_flags = SA_NOCLDWAIT;
    sigaction(SIGCHLD, &sigchld_action, NULL);
}


static void termSignalHandler(int signum)
{
    fprintf(stdout,"\nReceived terminate signal. Exiting...\n");
    syslog (LOG_NOTICE, "Received terminate signal. Exiting...");
    // StopNonBlocking(0);
    // if (s_manger) s_manger->stopWriteToCSV();
    g_run = false;
}


static void installTermSignalHandler()
{
    struct sigaction action;
    memset(&action, 0, sizeof(struct sigaction));
    action.sa_handler = termSignalHandler;
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
}

std::string exec(const char* cmd) {
    char buffer[128];
    std::string result = "";
    FILE* pipe = popen(cmd, "r");
    if (!pipe) throw std::runtime_error("popen() failed!");
    try {
        while (fgets(buffer, sizeof buffer, pipe) != NULL) {
            result += buffer;
        }
    } catch (...) {
        pclose(pipe);
        throw;
    }
    pclose(pipe);
    return result;
}

int main(int argc, char *argv[])
{
     // Open logging into "/var/log/messages" or /var/log/syslog" or other configured...
    setlogmask (LOG_UPTO (LOG_INFO));
    openlog ("streaming-server", LOG_CONS | LOG_PID | LOG_NDELAY, LOG_LOCAL1);

    g_argv0 = argv[0];
    auto opt = ClientOpt::parse(argc,argv);

#ifndef Z20
    rp_CalibInit();
    auto osc_calib_params = rp_GetCalibrationSettings();
#endif

    int32_t  ch1_off = 0;
    int32_t  ch2_off = 0;
    float    ch1_gain = 1;
    float    ch2_gain = 1;
    bool     filterBypass = true;
	uint32_t aa_ch1 = 0;
	uint32_t bb_ch1 = 0;
	uint32_t kk_ch1 = 0xFFFFFF;
	uint32_t pp_ch1 = 0;
	uint32_t aa_ch2 = 0;
	uint32_t bb_ch2 = 0;
	uint32_t kk_ch2 = 0xFFFFFF;
	uint32_t pp_ch2 = 0;


    if (opt.background){
        FILE *fp= NULL;
        pid_t process_id = 0;
        pid_t sid = 0;
        // Create child process
        process_id = fork();
        // Indication of fork() failure
        if (process_id < 0)
        {
            fprintf(stderr,"fork failed!\n");
            // Return failure in exit status
            exit(1);
        }

        // PARENT PROCESS. Need to kill it.
        if (process_id > 0)
        {
            //printf("process_id of child process %d \n", process_id);
            // return success in exit status
            exit(0);
        }

        //unmask the file mode
        umask(0);
        //set new session
        sid = setsid();
        if(sid < 0)
        {
            // Return failure
            exit(1);
        }

        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
    }


    try{
        auto hosts = exec("ip addr show eth0 2> /dev/null");
        auto whosts = exec("ip addr show wlan0 2> /dev/null");
        std::regex pattern("[0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3}");
        auto v_hosts = ClientOpt::split(hosts,'\n');
        auto v_whosts = ClientOpt::split(whosts,'\n');
        v_hosts.insert(std::end(v_hosts), std::begin(v_whosts), std::end(v_whosts));
        std::string brchost; 
        for(auto h: v_hosts){
            std::smatch match;
            if(std::regex_search(h, match, pattern)) {
                if (match.size() > 0){
                    if (brchost != "") brchost += ";";
                    brchost += match[match.size()-1];
                }
            }
        }
        
       	#ifdef STREAMING_MASTER
			auto mode = asionet_broadcast::CAsioBroadcastSocket::ABMode::AB_SERVER_MASTER;
		#endif
        #ifdef STREAMING_SLAVE
			auto mode = asionet_broadcast::CAsioBroadcastSocket::ABMode::AB_SERVER_SLAVE;
        #endif 

        #ifdef Z10
		asionet_broadcast::CAsioBroadcastSocket::Model model = asionet_broadcast::CAsioBroadcastSocket::Model::RP_125_14;
		#endif

		#ifdef Z20
		asionet_broadcast::CAsioBroadcastSocket::Model model = asionet_broadcast::CAsioBroadcastSocket::Model::RP_122_16;
		#endif

		#ifdef Z20_125
		asionet_broadcast::CAsioBroadcastSocket::Model model = asionet_broadcast::CAsioBroadcastSocket::Model::RP_125_14_Z20;
		#endif

		#ifdef Z20_250_12
		asionet_broadcast::CAsioBroadcastSocket::Model model = asionet_broadcast::CAsioBroadcastSocket::Model::RP_250_12;
		#endif

		con_server = std::make_shared<ServerNetConfigManager>(opt.conf_file,mode,"127.0.0.1",opt.config_port);
        con_server->startBroadcast(model, brchost,opt.broadcast_port);

        con_server->addHandler(ServerNetConfigManager::Events::GET_NEW_SETTING,[](){
            std::lock_guard<std::mutex> lock(g_print_mtx);
            fprintf(stdout, "Get new settings\n");
            RP_LOG (LOG_INFO,"Get new settings\n");
            //updateUI();
        });

        con_server->addHandler(ServerNetConfigManager::Events::START_STREAMING,[](){
            //StartServer();
        });

        con_server->addHandler(ServerNetConfigManager::Events::STOP_STREAMING,[](){
            //StopNonBlocking(0);
        });
        
    }catch (std::exception& e)
    {
        fprintf(stderr, "Error: Init ServerNetConfigManager() %s\n",e.what());
        RP_LOG (LOG_ERR,"Error: Init ServerNetConfigManager() %s\n",e.what());
        exit(EXIT_FAILURE);
    }

   
//     int resolution = 1;
//     int channel = 3;
//     int protocol = 1;
//     int sock_port = 8900;
//     std::string ip_addr_host = "127.0.0.1";
//     int format = 0;
//     int rate = 1;
//     bool use_file = false;
//     int samples = -1;
//     int save_mode = 1;
//     bool use_calib = false;
//     int attenuator = 0;
//     int ac_dc = 0;

//     try{
//         ifstream file(filepath);
//         std::string key;
//         std::string value;

// 	if (!file.good()) throw std::exception();

//         while (file >> key >> value) {
//             if ("host" == key) {
//                 ip_addr_host = value;
//                 continue;
//             }
//             if ("port" == key) {
//                 sock_port = stoi(value);
//                 continue;
//             }
//             if ("protocol" == key) {
//                 protocol = stoi(value);
//                 continue;
//             }
//             if ("rate" == key) {
//                 rate = stoi(value);
//                 continue;
//             }
//             if ("resolution" == key) {
//                 resolution = stoi(value);
//                 continue;
//             }
//             if ("use_file" == key) {
//                 use_file = (bool)stoi(value);
//                 continue;
//             }
//             if ("format" == key) {
//                 format = stoi(value);
//                 continue;
//             }
//             if ("samples" == key) {
//                 samples = stoi(value);
//                 continue;
//             }
//             if ("channels" == key) {
//                 channel = stoi(value);
//                 continue;
//             }
//             if ("save_mode" == key) {
//                 save_mode = stoi(value);
//                 continue;
//             }
// #ifndef Z20
//             if ("attenuator" == key) {
//                 attenuator = stoi(value);
//                 continue;
//             }
//             if ("use_calib" == key) {
//                 use_calib = (bool)(stoi(value) - 1);
//                 continue;
//             }
// #endif

// #ifdef Z20_250_12
//             if ("coupling" == key) {
//                 ac_dc = stoi(value);
//                 continue;
//             }
// #endif
//             throw std::exception();
//         }
//     }catch(std::exception& e)
//     {
//         fprintf(stderr,  "Error loading configuration file\n");
//         RP_LOG (LOG_ERR, "Error loading configuration file");
//         exit(1);
//     }


    fprintf(stdout,"streaming-server started\n");
    RP_LOG (LOG_NOTICE, "streaming-server started");

    installTermSignalHandler();
    // Handle close child events
    handleCloseChildEvents();

    try {
		CStreamingManager::MakeEmptyDir(FILE_PATH);
	}catch (std::exception& e)
	{
        fprintf(stderr,  "Error: Can't create %s dir %s",FILE_PATH,e.what());
        RP_LOG (LOG_ERR, "Error: Can't create %s dir %s",FILE_PATH,e.what());
        exit(EXIT_FAILURE);
	}

    while(g_run){
    }

    try{

//     if (use_calib) {
// #ifdef Z20_250_12
//         if (attenuator == 1) {
//             if (ac_dc == 1) {
//                 ch1_gain = calibFullScaleToVoltage(osc_calib_params.osc_ch1_g_1_ac) / 20.0;  // 1:1
//                 ch2_gain = calibFullScaleToVoltage(osc_calib_params.osc_ch2_g_1_ac) / 20.0;  // 1:1
//                 ch1_off  = osc_calib_params.osc_ch1_off_1_ac; 
//                 ch2_off  = osc_calib_params.osc_ch2_off_1_ac;
//             }
//             else {
//                 ch1_gain = calibFullScaleToVoltage(osc_calib_params.osc_ch1_g_1_dc) / 20.0;  // 1:1
//                 ch2_gain = calibFullScaleToVoltage(osc_calib_params.osc_ch2_g_1_dc) / 20.0;  // 1:1
//                 ch1_off  = osc_calib_params.osc_ch1_off_1_dc; 
//                 ch2_off  = osc_calib_params.osc_ch2_off_1_dc; 
//             }
//         }else{
//             if (ac_dc == 1) {
//                 ch1_gain = calibFullScaleToVoltage(osc_calib_params.osc_ch1_g_20_ac);  // 1:20
//                 ch2_gain = calibFullScaleToVoltage(osc_calib_params.osc_ch2_g_20_ac);  // 1:20
//                 ch1_off  = osc_calib_params.osc_ch1_off_20_ac;
//                 ch2_off  = osc_calib_params.osc_ch2_off_20_ac;
//             } else {
//                 ch1_gain = calibFullScaleToVoltage(osc_calib_params.osc_ch1_g_20_dc);  // 1:20
//                 ch2_gain = calibFullScaleToVoltage(osc_calib_params.osc_ch2_g_20_dc);  // 1:20
//                 ch1_off  = osc_calib_params.osc_ch1_off_20_dc;
//                 ch2_off  = osc_calib_params.osc_ch2_off_20_dc;
//             }
//         }
// #endif

// #if defined Z10 || defined Z20_125
//         filterBypass = false;
//         if (attenuator == 1) {
//             ch1_gain = calibFullScaleToVoltage(osc_calib_params.fe_ch1_fs_g_lo) / 20.0;  
//             ch2_gain = calibFullScaleToVoltage(osc_calib_params.fe_ch2_fs_g_lo) / 20.0;  
//             ch1_off  = osc_calib_params.fe_ch1_lo_offs;
//             ch2_off  = osc_calib_params.fe_ch2_lo_offs;
//             aa_ch1 = osc_calib_params.low_filter_aa_ch1;
//             bb_ch1 = osc_calib_params.low_filter_bb_ch1;
//             pp_ch1 = osc_calib_params.low_filter_pp_ch1;
//             kk_ch1 = osc_calib_params.low_filter_kk_ch1;
//             aa_ch2 = osc_calib_params.low_filter_aa_ch2;
//             bb_ch2 = osc_calib_params.low_filter_bb_ch2;
//             pp_ch2 = osc_calib_params.low_filter_pp_ch2;
//             kk_ch2 = osc_calib_params.low_filter_kk_ch2;
//         }else{
//             ch1_gain = calibFullScaleToVoltage(osc_calib_params.fe_ch1_fs_g_hi);  
//             ch2_gain = calibFullScaleToVoltage(osc_calib_params.fe_ch2_fs_g_hi);  
//             ch1_off  = osc_calib_params.fe_ch1_hi_offs;
//             ch2_off  = osc_calib_params.fe_ch2_hi_offs;
//             aa_ch1 = osc_calib_params.hi_filter_aa_ch1;
//             bb_ch1 = osc_calib_params.hi_filter_bb_ch1;
//             pp_ch1 = osc_calib_params.hi_filter_pp_ch1;
//             kk_ch1 = osc_calib_params.hi_filter_kk_ch1;
//             aa_ch2 = osc_calib_params.hi_filter_aa_ch2;
//             bb_ch2 = osc_calib_params.hi_filter_bb_ch2;
//             pp_ch2 = osc_calib_params.hi_filter_pp_ch2;
//             kk_ch2 = osc_calib_params.hi_filter_kk_ch2;
//         }
// #endif 
//     }

// #ifdef Z20_250_12
// //    rp_spi_fpga::rp_spi_load_via_fpga("/opt/redpitaya/lib/configs/AD9613BCPZ-250_streaming.xml");
//     rp_max7311::rp_setAttenuator(RP_MAX7311_IN1, attenuator == 1  ? RP_ATTENUATOR_1_1 : RP_ATTENUATOR_1_20);
//     rp_max7311::rp_setAttenuator(RP_MAX7311_IN2, attenuator == 1  ? RP_ATTENUATOR_1_1 : RP_ATTENUATOR_1_20);
//     rp_max7311::rp_setAC_DC(RP_MAX7311_IN1, ac_dc == 1 ? RP_AC_MODE : RP_DC_MODE);
//     rp_max7311::rp_setAC_DC(RP_MAX7311_IN2, ac_dc == 1 ? RP_AC_MODE : RP_DC_MODE);
// #endif
        
//         std::vector<UioT> uioList = GetUioList();
//         // Search oscilloscope
//         auto file_type = Stream_FileType::WAV_TYPE;

//         for (const UioT &uio : uioList)
//         {
//             if (uio.nodeName == "rp_oscilloscope")
//             {
//                 // TODO start server;
//                 osc = COscilloscope::Create(uio, (channel ==1 || channel == 3) , (channel ==2 || channel == 3) , rate);
//                 osc->setCalibration(ch1_off,ch1_gain,ch2_off,ch2_gain);
//                 osc->setFilterCalibrationCh1(aa_ch1,bb_ch1,kk_ch1,pp_ch1);
//                 osc->setFilterCalibrationCh2(aa_ch2,bb_ch2,kk_ch2,pp_ch2);
//                 osc->setFilterBypass(filterBypass);
//                 break;
//             }
//         }


//         if (use_file == false) {
//             s_manger = CStreamingManager::Create(
//                     ip_addr_host,
//                     std::to_string(sock_port).c_str(),
//                     protocol == 1 ? asionet::Protocol::TCP : asionet::Protocol::UDP);
//         }else{
// 	    	if (format == 1) file_type = Stream_FileType::TDMS_TYPE;
//     		if (format == 2) file_type = Stream_FileType::CSV_TYPE;
//             s_manger = CStreamingManager::Create(file_type , FILE_PATH , samples, save_mode == 2);
//             s_manger->notifyStop = [](int status)
//                                 {
//                                     StopNonBlocking(0);                                   
//                                 };
//         }
//         int resolution_val = (resolution == 1 ? 8 : 16);
//         s_app = new CStreamingApplication(s_manger, osc, resolution_val, rate, channel, attenuator , 16);
//         s_app->run();
//         delete s_app;
//         if (file_type == Stream_FileType::CSV_TYPE) s_manger->convertToCSV();
    }catch (std::exception& e)
    {
        fprintf(stderr, "Error: main() %s\n",e.what());
        RP_LOG(LOG_ERR, "Error: main() %s\n",e.what());
    }
    con_server->stop();
    fprintf(stdout,  "streaming-server stopped.\n");
    RP_LOG(LOG_INFO, "streaming-server stopped.");
    closelog ();
    return (EXIT_SUCCESS);
}

void StopServer(int x){
	try{
		if (s_app!= nullptr){
			s_app->stop(false);
		}
	}catch (std::exception& e){
		fprintf(stderr, "Error: StopServer() %s\n",e.what());
        RP_LOG(LOG_ERR, "Error: StopServer() %s\n",e.what());
	}
}

void StopNonBlocking(int x){
	try{
		std::thread th(StopServer ,x);
		th.detach();
	}catch (std::exception& e){
		fprintf(stderr, "Error: StopNonBlocking() %s\n",e.what());
        RP_LOG(LOG_ERR, "Error: StopNonBlocking() %s\n",e.what());
	}
}


