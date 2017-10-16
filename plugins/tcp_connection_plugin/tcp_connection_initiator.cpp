#include <eos/tcp_connection_plugin/tcp_connection_initiator.hpp>

#include <fc/log/logger.hpp>

#include <boost/algorithm/string/erase.hpp>

namespace eos {

static void parse_ip_port(const std::string& in, std::string& ip, std::string& port) {
   size_t last_colon = in.find_last_of(':');

   //(naively) handle the case user didn't specify port; we'll not touch the passed port parameter
   if(last_colon == std::string::npos)
      ip = in;
   else if(last_colon && in.find('[') != std::string::npos && in.find_last_of(']') != last_colon-1)
      ip = in;
   else {
      ip = in.substr(0, last_colon);
      port = in.substr(last_colon+1);
   }
   boost::erase_all(ip, "[");
   boost::erase_all(ip, "]");
}

tcp_connection_initiator::tcp_connection_initiator(const boost::program_options::variables_map& options, boost::asio::io_service& i) :
   strand(i), ios(i) {

   for(const std::string& bindaddr : options["listen-endpoint"].as<std::vector<std::string>>()) {
      std::string ip;
      std::string port{"9876"};
      parse_ip_port(bindaddr, ip, port);

      boost::asio::ip::tcp::resolver resolver(ios);
      boost::asio::ip::tcp::resolver::query query(ip, port);

      try {
         boost::asio::ip::tcp::resolver::iterator addresses = resolver.resolve(query);
         acceptors.emplace_front(ios, addresses->endpoint());
      }
      catch(boost::system::system_error sr) {
        elog("Failed to listen on an incoming port-- ${sr}", ("sr", sr.what()));
      }
   }


   outgoing_connections.emplace_front(ios, boost::asio::ip::tcp::endpoint(boost::asio::ip::address::from_string("127.0.0.1"), 6666));
}

void tcp_connection_initiator::go() {
   for(active_acceptor& aa : acceptors) {
      aa.acceptor.listen();
      accept_connection(aa);
   }

   for(outgoing_attempt& oa : outgoing_connections)
      retry_connection(oa);
}

void tcp_connection_initiator::accept_connection(active_acceptor& aa) {
   aa.acceptor.async_accept(aa.socket_to_accept, strand.wrap([this,&aa](auto ec) {
      handle_incoming_connection(ec, aa);
   }));
}

 void tcp_connection_initiator::handle_incoming_connection(const boost::system::error_code& ec, active_acceptor& aa) {
   ilog("accepted connection");
  
   //XX verify on some sort of acceptable incoming IP range
   active_connections.emplace_front(std::make_shared<tcp_connection>((std::move(aa.socket_to_accept))));
   std::list<active_connection>::iterator new_conn_it = active_connections.begin();

   new_conn_it->connection_failure_connection = new_conn_it->connection->connect_on_disconnected(strand.wrap([this,new_conn_it]() {
      new_conn_it->connection_failure_connection.disconnect();
      active_connections.erase(new_conn_it);
    }));
  
   accept_connection(aa);
}
void tcp_connection_initiator::retry_connection(outgoing_attempt& outgoing) {
   outgoing.outgoing_socket = boost::asio::ip::tcp::socket(outgoing.reconnect_timer.get_io_service());
   outgoing.outgoing_socket.async_connect(outgoing.remote_endpoint, strand.wrap([this,&outgoing](auto ec) {
      outgoing_connection_complete(ec, outgoing);
   }));
}

void tcp_connection_initiator::outgoing_connection_complete(const boost::system::error_code& ec, outgoing_attempt& outgoing) {
   if(ec) {
      ilog("connection failed");
      outgoing.reconnect_timer.expires_from_now(std::chrono::seconds(3));
      outgoing.reconnect_timer.async_wait([this,&outgoing](auto ec){
         if(ec)
            throw std::runtime_error("Unexpected timer wait failure");
         retry_connection(outgoing);
      });
   }
   else {
      ilog("Connection good!");
      outgoing.reconnect_timer.cancel();
      active_connections.emplace_front(std::make_shared<tcp_connection>(std::move(outgoing.outgoing_socket)));
      std::list<active_connection>::iterator new_conn_it = active_connections.begin();

      new_conn_it->connection_failure_connection = new_conn_it->connection->connect_on_disconnected(strand.wrap([this,new_conn_it,&outgoing]() {
         new_conn_it->connection_failure_connection.disconnect();
         active_connections.erase(new_conn_it);
         retry_connection(outgoing);
       }));
     }
 }

}