/*
	Node / map piece server

	The majority of the ZMQ stuff is based on examples from the ZMQ guide:
	http://zguide.zeromq.org/chapter:all
	Most notably the pub/sub message enveloping example from
	https://github.com/imatix/zguide/blob/master/examples/C++/psenvpub.cpp
	https://github.com/imatix/zguide/blob/master/examples/C++/psenvsub.cpp
*/

#include "antix.cpp"

using namespace std;

string master_host = "localhost";
string master_node_port = "7770";
string master_publish_port = "7773";

int my_id;
double world_size;
double my_min_x;
// where neighbour begins
double my_max_x;
int sleep_time;
int initial_puck_amount;

vector<Puck> pucks;
vector<Robot> robots;
vector<Puck> left_foreign_pucks;
vector<Robot> left_foreign_robots;
vector<Puck> right_foreign_pucks;
vector<Robot> right_foreign_robots;

antixtransfer::Node_list::Node left_node;
antixtransfer::Node_list::Node right_node;

// Connect to master & identify ourselves. Get state
zmq::socket_t *master_control_sock;
// Master publishes list of nodes to us when beginning simulation
zmq::socket_t *master_publish_sock;
// neighbours publish foreign entities & move bot msgs on these
zmq::socket_t *left_sub_sock;
zmq::socket_t *right_sub_sock;
// we publish our border entities to these sockets
zmq::socket_t *left_pub_sock;
zmq::socket_t *right_pub_sock;
// control socket to service client commands
zmq::socket_t *control_sock;
// control sockets to speak to neighbours. useful for synchronization
//zmq::socket_t *left_control_sock;
//zmq::socket_t	*right_control_sock;

/*
	Find our offset in node_list and set my_min_x, my_max_x
*/
void
set_dimensions(antixtransfer::Node_list *node_list) {
	antixtransfer::Node_list::Node *node;
	double offset_size = world_size / node_list->node_size();

	for (int i = 0; i < node_list->node_size(); i++) {
		node = node_list->mutable_node(i);
		if (node->id() == my_id) {
			my_min_x = node->x_offset();
			my_max_x = my_min_x + offset_size;
			return;
		}
	}
}

/*
	Place pucks randomly within our region
*/
void
generate_pucks() {
	for (int i = 0; i < initial_puck_amount; i++) {
		pucks.push_back( Puck(my_min_x, my_max_x, world_size) );
	}
}

/*
	Given a map which may or may not contain entities, add any entities therein
	to our internal records of foreign robots & pucks
*/
void
update_foreign_entities(antixtransfer::SendMap *map, vector<Robot> *foreign_robots, vector<Puck> *foreign_pucks) {
	foreign_robots->clear();
	foreign_pucks->clear();
	// foreign robots
	for (int i = 0; i < map->robot_size(); i++) {
		foreign_robots->push_back( Robot( map->robot(i).x(), map->robot(i).y(), map->robot(i).id(), map->robot(i).team() ) );
	}
	// foreign pucks
	for (int i = 0; i < map->puck_size(); i++) {
		foreign_pucks->push_back( Puck( map->puck(i).x(), map->puck(i).y(), map->puck(i).held() ) );
	}
}

/*
	Send entities which are within sight distance of our left border
	and within sight distance of our right border
*/
void
send_border_entities() {
	// container for robots & pucks
	antixtransfer::SendMap send_map;
	// XXX Right now we only have one big list of robots, so all are sent
	for(vector<Robot>::iterator it = robots.begin(); it != robots.end(); it++) {
		antixtransfer::SendMap::Robot *robot = send_map.add_robot();
		robot->set_x( it->x );
		robot->set_y( it->y );
		// not implemented
		robot->set_team(0);
	}

	// XXX only one list of pucks right now
	for(vector<Puck>::iterator it = pucks.begin(); it != pucks.end(); it++) {
		antixtransfer::SendMap::Puck *puck = send_map.add_puck();
		puck->set_x( it->x );
		puck->set_y( it->y );
		puck->set_held( it->held );
	}

	cout << "Sending " << send_map.puck_size() << " pucks and " << send_map.robot_size() << " robots to our neighbours..." << endl;
	zmq::message_t type_left(2);
	memcpy(type_left.data(), "f", 2);
	left_pub_sock->send(type_left, ZMQ_SNDMORE);
	antix::send_pb(left_pub_sock, &send_map);

	zmq::message_t type_right(2);
	memcpy(type_right.data(), "f", 2);
	right_pub_sock->send(type_right, ZMQ_SNDMORE);
	antix::send_pb(right_pub_sock, &send_map);
	cout << "Sent border entities to neighbours." << endl;
}

/*
	Know there is a message waiting from a neighbour. Handle it
*/
void
parse_neighbour_message(zmq::socket_t *sub_sock, zmq::message_t *type_msg, vector<Robot> *foreign_robots, vector<Puck> *foreign_pucks) {
	// First we read the envelope address (specifies from neighbour or a client)
	// type_msg contains this
	string s = string((char *) type_msg->data());
	cout << "Received node control message: " << s << endl;

	// Foreign entity update
	if (s == "f") {
		antixtransfer::SendMap map_foreign;
		antix::recv_pb(sub_sock, &map_foreign, 0);
		update_foreign_entities(&map_foreign, foreign_robots, foreign_pucks);
		cout << "Received " << map_foreign.puck_size() << " pucks and " << map_foreign.robot_size() << " robots from a neighbour" << endl;
	}
	// Move bot
	else if (s == "m") {
		antixtransfer::RequestRobotTransfer move_msg;
		antix::recv_pb(sub_sock, &move_msg, 0);
		Robot r(move_msg.x(), move_msg.y(), move_msg.id(), move_msg.team());
		r.a = move_msg.a();
		r.v = move_msg.v();
		r.w = move_msg.w();
		r.has_puck = move_msg.has_puck();
		// If the robot is carrying a puck, we have to add a puck to our records
		if (r.has_puck) {
			Puck p(r.x, r.y, true);
			pucks.push_back(p);
			r.puck = &p;
		}
		robots.push_back(r);
		cout << "Robot transferred to this node." << endl;
	}
}

/*
	Clear old foreign robots/pucks and attempt to re-fill based on msgs from our
	neighbours
*/
void
recv_neighbour_messages() {
	zmq::message_t type_msg;
	// Not really fair, but good enough for now
	// (as we first do all of the left node's, then the right's
	while (left_sub_sock->recv(&type_msg, ZMQ_NOBLOCK) == 1) {
		parse_neighbour_message(left_sub_sock, &type_msg, &left_foreign_robots, &left_foreign_pucks);
	}
	while (right_sub_sock->recv(&type_msg, ZMQ_NOBLOCK) == 1) {
		parse_neighbour_message(right_sub_sock, &type_msg, &right_foreign_robots, &right_foreign_pucks);
	}
}

/*
	Remove the puck that robot r is carrying, if it is carrying one
*/
void
remove_puck(Robot *r) {
	if (!r->has_puck)
		return;
	for (vector<Puck>::iterator it = pucks.begin(); it != pucks.end(); it++) {
		if (it->robot == r) {
			pucks.erase(it);
			break;
		}
	}
}

/*
	Robot has been found to be outside of our map portion
	Transfer the robot (and possibly its puck) to the given node
	Note: Removal of robot from local records takes place elsewhere
*/
void
move_robot(Robot *r) {
	antixtransfer::Node_list::Node *node;
	zmq::socket_t *pub_sock;

	if (r->x < my_min_x) {
		node = &left_node;
		pub_sock = left_pub_sock;
	} else {
		node = &right_node;
		pub_sock = right_pub_sock;
	}
	cout << "Here" << endl;

	// transfer the robot
	antixtransfer::RequestRobotTransfer transfer_msg;
	transfer_msg.set_id(r->id);
	transfer_msg.set_team(r->team);
	transfer_msg.set_x(r->x);
	transfer_msg.set_y(r->y);
	transfer_msg.set_a(r->a);
	transfer_msg.set_v(r->v);
	transfer_msg.set_w(r->w);
	transfer_msg.set_has_puck(r->has_puck);

	// First we send the address/type message
	zmq::message_t type(2);
	memcpy(type.data(), "m", 2);
	pub_sock->send(type, ZMQ_SNDMORE);
	cout << "Here2" << endl;
	// Then we send our content msg
	antix::send_pb(pub_sock, &transfer_msg);
	cout << "Here3" << endl;

	// Wait for response
	//antixtransfer::TransferUpdate response;
	//antix::recv_pb(node_control_sock, &response, 0);

	cout << "Transferred robot " << r->id << " on team " << r->team << endl;
}

/*
	update the pose of a single robot
	Taken from rtv's Antix
*/
void
update_pose(Robot *r) {
	double dx = r->v * cos(r->a);
	double dy = r->v * sin(r->a);
	double da = r->w;

	r->x = antix::DistanceNormalize(r->x + dx, world_size);
	r->y = antix::DistanceNormalize(r->y + dy, world_size);
	r->a = antix::AngleNormalize(r->a + da);

	// If we're holding a puck, it must move also
	if (r->has_puck) {
		r->puck->x = r->x;
		r->puck->y = r->y;
	}

	// XXX deal with collision
}

/*
	Go through our local robots & update their poses
*/
void
update_poses() {
	// For each robot, update its pose
	for(vector<Robot>::iterator it = robots.begin(); it != robots.end(); it++) {
		update_pose(&*it);

		// Then check each robot for being outside of our range
		// separate from update_pose() as we must be careful deleting from vector
		// while using an iterator
		if (it->x < my_min_x || it->x >= my_max_x) {
			cout << "Robot " << it->id << " team " << it->team << " out of range, moving..." << endl;
			// Remove puck if robot is carrying one
			remove_puck(&*it);
			move_robot(&*it);
			it = robots.erase(it);
		}
	}
	cout << "Poses updated for all robots." << endl;
}

/*
	Initiate connections to our neighbour's control sockets
*/
/*
void
connect_neighbour_control_socks(zmq::context_t *context) {
	left_control_sock = new zmq::socket_t(*context, ZMQ_REQ);
	right_control_sock = new zmq::socket_t(*context, ZMQ_REQ);
	left_control_sock->connect(antix::make_endpoint(left_node.ip_addr(), left_node.control_port()));
	right_control_sock->connect(antix::make_endpoint(right_node.ip_addr(), right_node.control_port()));
}
*/

/*
	Wait for a response from our neighbours which indicates synchronization
	Without this messages are lost on the neighbour_publish_socket

	XXX Not absolutely correct? Nodes can still start slightly unsynced - although
	this is an improvement

	This is based on the example from
	http://github.com/imatix/zguide/blob/master/examples/C%2B%2B/syncsub.cpp
*/
/*
void
synchronize_neighbours(zmq::context_t *context, zmq::socket_t *control_sock) {
	// First we send a blank message to both of our neighbours
	zmq::message_t blank_left(1);
	left_control_sock->send(blank_left);
	zmq::message_t blank_right(1);
	right_control_sock->send(blank_right);

	int acks_received = 0;
	int syncs_responded = 0;
	// same polling as in master
	zmq::pollitem_t items[] = {
		{ *control_sock, 0, ZMQ_POLLIN, 0},
		{ *left_control_sock, 0, ZMQ_POLLIN, 0},
		{ *right_control_sock, 0, ZMQ_POLLIN, 0}
	};
	// Then we wait for a response & respond to those sent to us
	while (acks_received < 2 || syncs_responded < 2) {
		zmq::message_t response;
		zmq::poll(&items [0], 3, -1);

		// sync requested on control port
		if (items[0].revents & ZMQ_POLLIN) {
			cout << "Received sync request. Sending response..." << endl;
			control_sock->recv(&response);
			zmq::message_t blank(1);
			control_sock->send(blank);
			syncs_responded++;
		}
		// left sync response
		if (items[1].revents & ZMQ_POLLIN) {
			cout << "Received sync response from left node" << endl;
			left_control_sock->recv(&response);
			acks_received++;
		}
		// right sync response
		if (items[2].revents & ZMQ_POLLIN) {
			cout << "Received sync response from right node" << endl;
			right_control_sock->recv(&response);
			acks_received++;
		}
	}
}
*/

/*
	Service control messages from clients
*/
void
service_control_messages() {
	return;
	cout << "Receiving a client control message..." << endl;
	antixtransfer::control_message msg;
	antix::recv_pb(control_sock, &msg, 0);
	cout << "Done: Received a client control message" << endl;
	cout << "XXX not sending response yet!" << endl;
	// XXX must send response or die
}

/*
	Control messages from neighbouring nodes
	move_bot messages
*/
/*
void
node_control(zmq::socket_t *control_sock, string *type) {
	cout << "Received a node control message" << endl;
	antixtransfer::RequestRobotTransfer transfer_msg;
	antix::recv_pb(control_sock, &transfer_msg, 0);

	// Add the robot to our records
	Robot r(transfer_msg.x(), transfer_msg.y(), transfer_msg.id(), transfer_msg.team());
	r.a = transfer_msg.a();
	r.v = transfer_msg.v();
	r.w = transfer_msg.w();
	r.has_puck = transfer_msg.has_puck();
	// If the robot is carrying a puck, we have to add a puck to our records
	if (r.has_puck) {
		Puck p(r.x, r.y, true);
		pucks.push_back(p);
		r.puck = &p;
	}
	robots.push_back(r);
	cout << "Robot transferred to this node." << endl;
	cout << "Current robots:" << endl;
	for (vector<Robot>::iterator it = robots.begin(); it != robots.end(); it++) {
		cout << "Robot id " << it->id << " on team " << it->team << " at " << it->x << ", " << it->y << endl;
	}
}
*/

/*
	add_bot, sense, setspeed, pickup, drop from client
	move_bot from neighbours
*/
/*
XXX example of recving enveloped
void
service_control_messages(zmq::socket_t *control_sock) {
	zmq::message_t type_msg;
	while(control_sock->recv(&type_msg, ZMQ_NOBLOCK) == 1) {
		// First we read the envelope address (specifies from neighbour or a client)
		// type_msg contains this
		string s = string((char *) type_msg.data());
		cout << "Received a control message: " << s << endl;

		// Then the actual message
		if (s == "c")
			client_control(control_sock);
		else if (s == "n")
			node_control(control_sock, &s);
	}
}
*/

int main(int argc, char **argv) {
	GOOGLE_PROTOBUF_VERIFY_VERSION;
	zmq::context_t context(1);
	
	if (argc != 5) {
		cerr << "Usage: " << argv[0] << " <IP to listen on> <left node port> <right node port> <control port>" << endl;
		return -1;
	}

	// socket to announce ourselves to master on
	master_control_sock = new zmq::socket_t(context, ZMQ_REQ);
	master_control_sock->connect(antix::make_endpoint(master_host, master_node_port));
	cout << "Connecting to master..." << endl;

	// socket to receive list of nodes on
	master_publish_sock = new zmq::socket_t(context, ZMQ_SUB);
	// subscribe to all messages on this socket: should just be a list of nodes
	master_publish_sock->setsockopt(ZMQ_SUBSCRIBE, "", 0);
	master_publish_sock->connect(antix::make_endpoint(master_host, master_publish_port));

	// send message announcing ourself. includes our own IP
	cout << "Sending master our existence notification..." << endl;

	// create & send pb msg identifying ourself
	antixtransfer::connect_init_node pb_init_msg;
	pb_init_msg.set_ip_addr( string(argv[1]) );
	pb_init_msg.set_left_port( string(argv[2]) );
	pb_init_msg.set_right_port( string(argv[3]) );
	pb_init_msg.set_control_port( string(argv[4]) );
	antix::send_pb(master_control_sock, &pb_init_msg);

	// receive message back stating our unique ID
	antixtransfer::connect_init_response init_response;
	antix::recv_pb(master_control_sock, &init_response, 0);
	my_id = init_response.id();
	world_size = init_response.world_size();
	sleep_time = init_response.sleep_time();
	initial_puck_amount = init_response.puck_amount();
	cout << "We are now node id " << my_id << endl;

	// receive node list
	// blocks until master publishes list of nodes
	antixtransfer::Node_list node_list;
	antix::recv_pb(master_publish_sock, &node_list, 0);
	cout << "Received list of nodes:" << endl;
	antix::print_nodes(&node_list);

	if (node_list.node_size() < 3) {
		cout << "Error: we need at least 3 nodes. Only received " << node_list.node_size() << " node(s)." << endl;
		return -1;
	}

	// calculate our min / max x from the offset assigned to us in node_list
	set_dimensions(&node_list);

	// find our left/right neighbours
	antix::set_neighbours(&left_node, &right_node, &node_list, my_id);
	cout << "Left neighbour id: " << left_node.id() << " " << left_node.ip_addr() << " left port " << left_node.left_port() << " right port " << left_node.right_port() << endl;
	cout << "Right neighbour id: " << right_node.id() << " " << right_node.ip_addr() << " left port " << right_node.left_port() << " right port " << right_node.right_port() << endl;

	// connect & subscribe to both neighbour's PUB sockets
	// these receive foreign entities that are near our border
	left_sub_sock = new zmq::socket_t(context, ZMQ_SUB);
	left_sub_sock->setsockopt(ZMQ_SUBSCRIBE, "", 0);
	left_sub_sock->connect(antix::make_endpoint(left_node.ip_addr(), left_node.right_port()));

	right_sub_sock = new zmq::socket_t(context, ZMQ_SUB);
	right_sub_sock->setsockopt(ZMQ_SUBSCRIBE, "", 0);
	right_sub_sock->connect(antix::make_endpoint(right_node.ip_addr(), right_node.left_port()));

	// open PUB socket neighbours where we publish entities close to the borders
	left_pub_sock = new zmq::socket_t(context, ZMQ_PUB);
	left_pub_sock->bind(antix::make_endpoint( argv[1], argv[2] ));
	right_pub_sock = new zmq::socket_t(context, ZMQ_PUB);
	right_pub_sock->bind(antix::make_endpoint( argv[1], argv[3] ));

	// create REP socket that receives control messages from clients
	control_sock = new zmq::socket_t(context, ZMQ_REP);
	control_sock->bind(antix::make_endpoint( argv[1], argv[4] ));

	// connect out to the control sockets of our neighbours
	//connect_neighbour_control_socks(&context);

	// Before we enter main loop, we must synchronize our connection to our
	// neighbours PUB sockets (neighbour_publish_sock), or else we risk losing
	// the initial messages
	// Buggy, interferes with control sockets currently
	//synchronize_neighbours(&context, &control_sock);

	// XXX test code
	//robots.push_back(Robot(0.5, 0.5, 1, 1));

	// generate pucks
	generate_pucks();
	cout << "Created " << pucks.size() << " pucks." << endl;
	for (vector<Puck>::iterator it = pucks.begin(); it != pucks.end(); it++) {
		cout << "Puck at " << it->x << "," << it->y << endl;
	}

	// enter main loop
	while (1) {
		// send entities within sight_range of our borders to our neighbours
		send_border_entities();

		// read from our neighbour SUB sockets
		// - update our foreign entity knowledge
		// - handle any move_bot messages
		recv_neighbour_messages();

		cout << "Current foreign entities: " << endl;
		for (vector<Robot>::iterator it = left_foreign_robots.begin(); it != left_foreign_robots.end(); it++)
			cout << "\tRobot at " << it->x << ", " << it->y << endl;
		for (vector<Robot>::iterator it = right_foreign_robots.begin(); it != right_foreign_robots.end(); it++)
			cout << "\tRobot at " << it->x << ", " << it->y << endl;
		for (vector<Puck>::iterator it = left_foreign_pucks.begin(); it != left_foreign_pucks.end(); it++)
			cout << "\tPuck at " << it->x << ", " << it->y << endl;
		for (vector<Puck>::iterator it = right_foreign_pucks.begin(); it != right_foreign_pucks.end(); it++)
			cout << "\tPuck at " << it->x << ", " << it->y << endl;

		// update poses for internal robots & move any robots outside our control
		update_poses();

		// service control messages on our REP socket
		service_control_messages();

		// sleep (code from rtv's Antix)
		usleep(sleep_time * 1e3);
	}

	google::protobuf::ShutdownProtobufLibrary();
	return 0;
}
