#include <iostream>

#include <so_5/all.hpp>

namespace example
{

// Header data for an operation.
//
// This type will also be used as a separate message.
struct operation_header_t
{
	int m_id;
	std::string m_header_data;

	operation_header_t( int id, std::string header_data )
		:	m_id{ id }
		,	m_header_data{ std::move(header_data) }
	{}
};

// Payload data for an operation.
struct operation_payload_t
{
	std::string m_payload_data;

	explicit operation_payload_t( std::string payload_data )
		:	m_payload_data{ std::move(payload_data) }
	{}
};

// Description of an operation.
//
// This type will be used as a message.
struct operation_t
{
	operation_header_t m_header;
	operation_payload_t m_payload;

	operation_t(
		int id,
		std::string header_data,
		std::string payload_data )
		:	m_header{ id, std::move(header_data) }
		,	m_payload{ std::move(payload_data) }
	{}
};

// Type of message to be sent to an operation processor agent.
struct handle_payload_t
{
	int m_id;
	std::string m_data;

	// Constructor extracts all necessary data from an operation_t object.
	handle_payload_t( const operation_t & op )
		:	m_id{ op.m_header.m_id }
		,	m_data{ op.m_payload.m_payload_data }
	{}
};

// An agent that handles operation_header_t messages only.
class a_op_registrator_t final : public so_5::agent_t
{
public:
	using so_5::agent_t::agent_t;

	void so_define_agent() override
	{
		so_subscribe_self().event( [](mhood_t<operation_header_t> cmd) {
				std::cout << "registering OP: " << cmd->m_id << " '"
						<< cmd->m_header_data << "'" << std::endl;
			} );
	}
};

// An agent that handles mutable handle_payload_t messages.
//
// There will be several such agents and each will have own name.
class a_op_processor_t final : public so_5::agent_t
{
	// Name of the processor to be shown.
	const std::string m_processor_name;

	// How long this processor should "work" on a message.
	const std::chrono::milliseconds m_processing_time;

public:
	a_op_processor_t(
		context_t ctx,
		std::string processor_name,
		std::chrono::milliseconds processing_time )
		:	so_5::agent_t{ std::move(ctx) }
		,	m_processor_name{ std::move(processor_name) }
		,	m_processing_time{ processing_time }
	{}

	void so_define_agent() override
	{
		so_subscribe_self().event(
			[this](mutable_mhood_t<handle_payload_t> cmd) {
				std::cout << m_processor_name << " processing started. OP: "
						<< cmd->m_id << " '" << cmd->m_data << "'" << std::endl;

				// Just suspend the current thread -- it's an imitation
				// of some long-lasting data processing.
				std::this_thread::sleep_for( m_processing_time );

				std::cout << m_processor_name << " processing finished. OP: "
						<< cmd->m_id << std::endl;
			} );
	}
};

// Type of an agent that generates operation_t messages.
//
// There will be several such agents and each will have own name.
class a_op_initiator_t final : public so_5::agent_t
{
	// Type of signal to be used as a periodic message.
	struct msg_time_to_generate final : public so_5::signal_t {};

	// Destination for generated messages.
	const so_5::mbox_t m_destination;

	// Name of this agent.
	const std::string m_initiator_name;

	// How often operation_t should be generated.
	const std::chrono::milliseconds m_generation_period;

	// Counter for operation IDs.
	int m_current_id;

	// ID of periodic msg_time_to_generate timer.
	so_5::timer_id_t m_generation_timer;

public:
	a_op_initiator_t(
		context_t ctx,
		so_5::mbox_t destination,
		std::string initiator_name,
		int base_id,
		std::chrono::milliseconds generation_period )
		:	so_5::agent_t{ std::move(ctx) }
		,	m_destination{ std::move(destination) }
		,	m_initiator_name{ std::move(initiator_name) }
		,	m_generation_period{ generation_period }
		,	m_current_id{ base_id }
	{}

	void so_define_agent() override
	{
		so_subscribe_self().event( &a_op_initiator_t::evt_time_to_generate );
	}

	void so_evt_start() override
	{
		// Periodic msg_time_to_generate has to be started.
		m_generation_timer = so_5::send_periodic< msg_time_to_generate >(
				*this,
				m_generation_period,
				m_generation_period );
	}

private:
	void evt_time_to_generate( mhood_t<msg_time_to_generate> )
	{
		so_5::send< operation_t >(
				m_destination,
				m_current_id++,
				"from: " + m_initiator_name,
				"data generated by: " + m_initiator_name );
	}
};

// Object of this type will be used for distribution of
// handle_payload_t messages between a_op_processor_t agents.
struct distribution_data_t
{
	// How many a_op_processor_t required.
	static constexpr std::size_t handler_count = 3;

	// Type of a container with op_processor's mboxes.
	using mbox_array_t = std::array< so_5::mbox_t, handler_count >;

	// Mboxes of a_op_processor_t agents to be used.
	mbox_array_t m_destinations;

	// This counter will be used for simple round-robin distribution scheme.
	std::atomic< std::size_t > m_current{ 0u };

	explicit distribution_data_t( mbox_array_t destinations )
		:	m_destinations{ destinations }
	{}
};

void make_coop( so_5::environment_t & env )
{
	env.introduce_coop(
		// All demo agents will work on separate threads.
		so_5::disp::active_obj::make_dispatcher( env ).binder(),
		[](so_5::coop_t & coop) {
			// Ordinary MPMC mbox to be used for operation_t messages.
			auto destination = coop.environment().create_mbox();

			// A couple of agents to generate operation_t messages.
			coop.make_agent< a_op_initiator_t >(
					destination,
					"Robert",
					0,
					std::chrono::milliseconds{ 125 } );
			coop.make_agent< a_op_initiator_t >(
					destination,
					"Garry",
					1'000'000,
					std::chrono::milliseconds{ 210 } );

			// Single registrator agent. We have to store its mbox to
			// use it in a message transformer.
			so_5::mbox_t registrator_mbox =
				coop.make_agent< a_op_registrator_t >()->so_direct_mbox();

			// An instance of distribution_data has to be created dynamically
			// and stored as shared_ptr to be used in a message transformer.
			auto distribution_data = std::make_shared< distribution_data_t >(
					// Create and store mboxes of op_processor agents.
					distribution_data_t::mbox_array_t{
						coop.make_agent< a_op_processor_t >(
								"Alice",
								std::chrono::milliseconds{ 150 } )->so_direct_mbox(),
						coop.make_agent< a_op_processor_t >(
								"Bob",
								std::chrono::milliseconds{ 250 } )->so_direct_mbox(),
						coop.make_agent< a_op_processor_t >(
								"Eve",
								std::chrono::milliseconds{ 200 } )->so_direct_mbox()
					} );

			// We need multi_sink_binding because a message has to
			// be processed by several message transformers.
			// Each transformer will be a message sink for operation_t message.
			auto * binding = coop.take_under_control(
					std::make_unique< so_5::multi_sink_binding_t<> >() );

			// The first and simplest transformer.
			// Takes operation_t and sends operation_t::m_header as
			// a separate message.
			so_5::bind_transformer( *binding, destination,
					[registrator_mbox]( const operation_t & msg ) {
						return so_5::make_transformed< operation_header_t >(
								registrator_mbox,
								msg.m_header );
					} );

			// The second and complex transformer.
			// Makes a new mutable message of type handle_payload_t and sends
			// it to a processor agent using round-robin distribution scheme.
			so_5::bind_transformer( *binding, destination,
					[distribution_data]( const operation_t & msg ) {
						// Which processor to use?
						const auto index = ( ++(distribution_data->m_current) ) %
								distribution_data->m_destinations.size();

						return so_5::make_transformed< so_5::mutable_msg< handle_payload_t > >(
								distribution_data->m_destinations[ index ],
								msg );
					} );
		} );
}

} /* namespace example */

int main()
{
	using namespace example;

	try
	{
		// Starting SObjectizer.
		so_5::launch(
			// A function for SO Environment initialization.
			[]( so_5::environment_t & env )
			{
				// Make a coop with demo agents.
				make_coop( env );

				// Give a couple of seconds to work.
				std::this_thread::sleep_for( std::chrono::seconds{ 2 } );

				// Stop the example.
				env.stop();
			},
			[]( so_5::environment_params_t & params )
			{
				// Turn message delivery tracing on to see message
				// transformations.
				params.message_delivery_tracer(
						so_5::msg_tracing::std_cerr_tracer() );
			} );
	}
	catch( const std::exception & ex )
	{
		std::cerr << "Error: " << ex.what() << std::endl;
		return 1;
	}

	return 0;
}
