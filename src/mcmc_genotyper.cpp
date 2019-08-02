#include "mcmc_genotyper.hpp"
#include "algorithms/count_walks.hpp"
#include "subgraph.hpp"
#include <algorithm> 
#include <random> 
#include <chrono>
#include <utility>
#include "multipath_alignment.hpp"

namespace vg {

    using namespace std;

    MCMCGenotyper::MCMCGenotyper(SnarlManager& snarls, VG& graph, const int n_iterations, const int seed):snarls(snarls), graph(graph), n_iterations(n_iterations), 
        seed(seed), random_engine(seed){
            
    
    }

    unique_ptr<PhasedGenome> MCMCGenotyper::run_genotype(const vector<MultipathAlignment>& reads, const double log_base) const{

        // set a flag for invalid contents so a message it observed 
        bool invalid_contents = false;

        // generate initial value 
        unique_ptr<PhasedGenome> genome = generate_initial_guess();
    
        for(int i = 1; i<= n_iterations; i++){
            
            // holds the previous sample allele
            double x_star_log = log_target(*genome, reads);
        
            // get contents from proposal_sample
            tuple<int, const Snarl*, vector<NodeTraversal> > to_receive = proposal_sample(*genome);
            int& modified_haplo = get<0>(to_receive);
             
            
            // if the to_receive contents are invalid, do not swap
            if (modified_haplo ==-1){
                invalid_contents = true;
                continue;
            }else{
                const Snarl& modified_site = *get<1>(to_receive); 
                vector<NodeTraversal>& old_allele = get<2>(to_receive); 
 
                // holds new sample allele
                double current_log = log_target(*genome, reads);

                // calculate likelihood ratio of posterior distribution 
                double likelihood_ratio = exp(log_base*(current_log - x_star_log));

                // calculate acceptance probability 
                double acceptance_probability = min(1.0, likelihood_ratio);
            
                if(generate_continuous_uniform(0.0,1.0) > acceptance_probability){ 
                    genome->set_allele(modified_site, old_allele.begin(), old_allele.end(), modified_haplo); 
                }
                
            }

        }
        if(invalid_contents){
            cerr<< "contents are invalid"<< endl;
        }
        cerr << "returning the genome from run_genotyper"<< endl;
        genome->print_phased_genome();
        return std::move(genome); 

    }   
    double MCMCGenotyper::log_target(PhasedGenome& phased_genome, const vector<MultipathAlignment>& reads)const{
        
        // sum of scores given the reads aligned on the haplotype 
        int32_t sum_scores = 0; 
        
              
        // condition on data 
        for(MultipathAlignment mp : reads){
            identify_start_subpaths(mp);  
            
            sum_scores += phased_genome.optimal_score_on_genome(mp, graph);
            cerr << "sum score is : "<< sum_scores <<endl;
        } 
        return sum_scores;
    }

    tuple<int, const Snarl*, vector<NodeTraversal> > MCMCGenotyper::proposal_sample(PhasedGenome& current)const{
        // get a different traversal through the snarl by uniformly choosing from all possible ways to traverse the snarl
        
        // bookkeeping: haplotype ID, snarl* (site that we replaced at), get_allele())
        tuple<int, const Snarl*, vector<NodeTraversal> > to_return;

        int& random_haplotype = get<0>(to_return);
        const Snarl*& random_snarl = get<1>(to_return);
        // the random path through the snarl 
        vector<NodeTraversal>& old_allele = get<2>(to_return);
        
        // sample uniformly between snarls 
        random_snarl = snarls.discrete_uniform_sample(random_engine);
        
        cerr << "this is the boundary nodes of the snarl " << random_snarl->start() << random_snarl->end() <<endl;
        
        if(random_snarl == nullptr){
            cerr << "random_snarl is null " <<endl;
            random_haplotype = -1;
            return to_return;
        }

        // get list of haplotypes that contain the snarl
        vector<id_t> matched_haplotypes = current.get_haplotypes_with_snarl(random_snarl);


        if(matched_haplotypes.empty()){
            cerr<< "matched haplotype is empty"<< endl;
            random_haplotype = -1;
            return to_return;
            
        }

        // choose a haplotype uiformly 
        id_t lower_bound = 0;
        id_t upper_bound = matched_haplotypes.size()-1;

        int random_num = generate_discrete_uniform(random_engine, lower_bound, upper_bound);
        random_haplotype = matched_haplotypes[random_num];


        pair<unordered_set<id_t>, unordered_set<edge_t> > contents = snarls.deep_contents(random_snarl, graph, true);

        // unpack the pair, we only care about the node_ids
        unordered_set<id_t>& ids = contents.first;
            
        // enumerate counts through nodes in snarl not the entire graph
        SubHandleGraph subgraph(&graph);
        
        for (id_t id : ids){
            
            // add each node from snarl in super graph to sub graph
            subgraph.add_handle(graph.get_handle(id, false));
        }
        
        
        // create a count_map of the subgraph
        auto count_contents = algorithms::count_walks_through_nodes(&subgraph);
        
        // unpack the count map from the count_contents 
        unordered_map<handle_t, size_t>& count_map = get<1>(count_contents);

        
        
        // create a topological order of the map
        vector<handle_t> topological_order = algorithms::lazier_topological_order(&graph);

        //  we want to get just the sink handle handle
        handle_t start = topological_order.back();  
        handle_t source = topological_order.front();
        
        // start at sink in topological
        bool start_from_sink =true;
        bool not_source = true;

        vector<NodeTraversal> allele;
 
        while(not_source){

            size_t  cum_sum = 0;
            vector<size_t> cumulative_sum;
            vector<size_t> paths_to_take;
            size_t  count = 0;
            vector<handle_t> handles;
            
            subgraph.follow_edges(start, start_from_sink, [&](const handle_t& next) { 
                unordered_map<handle_t, size_t>::iterator it;
                it = count_map.find(next); // find the handle
                count = it->second; // get the count 
                cum_sum += count;
                cumulative_sum.push_back(cum_sum); 
                handles.push_back(next); 
        
            });

            // choose a random path uniformly
            int l_bound = 0;
            int u_bound = cumulative_sum.back()-1;
            int random_num = generate_discrete_uniform(random_engine,l_bound, u_bound);

            // use the random_num to select a random_handle
            int found = 0, prev = 0;
            for (int i = 0; i< cumulative_sum.size() ; i++) {
                // check what range the random_num falls in    
                if (prev <= random_num && random_num < cumulative_sum[i] ){
                    found = i; // will correspond to the index of handles
                    break; 
                }
                prev = cumulative_sum[i];
            } 

            assert(found != -1);

            // start_ptr will point to random handle 
            start = handles[found]; 
            
            // save the random path 
            bool position = subgraph.get_is_reverse(start);
            Node* n = graph.get_node(subgraph.get_id(start));
            allele.push_back(NodeTraversal(n,position));

            // check if we are at the source, if so we terminate loop
            if(start == source){
                not_source = false;
            }    
            
        }
        
        old_allele = current.get_allele(*random_snarl, random_haplotype);
        
        

        // set new allele with random allele, replace with previous allele 
        current.set_allele(*random_snarl , allele.begin(), allele.end(), random_haplotype);
        
 
        return to_return;

    }
    int MCMCGenotyper::generate_discrete_uniform(minstd_rand0& random_engine, int lower_bound , int upper_bound) const{
        
        // choose a number randomly using discrete uniform distribution
        uniform_int_distribution<int> distribution(lower_bound, upper_bound);  
        int random_num = distribution(random_engine);

        return random_num;
    }
    double MCMCGenotyper::generate_continuous_uniform(const double a, const double b)const{
        
        uniform_real_distribution<double> distribution(a,b);
        double random_num = distribution(random_engine);
        
        return random_num;

    }
    unique_ptr<PhasedGenome> MCMCGenotyper::generate_initial_guess()const{
        
        unique_ptr<PhasedGenome> genome(new PhasedGenome(snarls));
        vector<NodeTraversal> haplotype; //will add twice  

        graph.for_each_path_handle([&](const path_handle_t& path){
        // capture all variables (paths) in scope by reference 

            if(!Paths::is_alt(graph.get_path_name(path))) {
            // If it isn't an alt path, we want to trace it
  
                for (handle_t handle : graph.scan_path(path)) {
                // For each occurrence from start to end
                    
                    // get the node and the node postion and add to the vector 
                    Node* node = graph.get_node(graph.get_id(handle));
                    bool position = graph.get_is_reverse(handle);
                    haplotype.push_back(NodeTraversal(node,position));
  
                }
            }
        });
        // construct haplotypes
        // haplotype1 = haplotype2
        genome->add_haplotype(haplotype.begin(), haplotype.end());
        genome->add_haplotype(haplotype.begin(), haplotype.end());

        // index sites
        genome->build_indices();

        return std::move(genome);
    }

}


