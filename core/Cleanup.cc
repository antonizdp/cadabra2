
#include "Cleanup.hh"
#include "Functional.hh"
#include "Algorithm.hh"
#include "algorithms/collect_terms.hh"
#include "algorithms/flatten_sum.hh"
#include "properties/Derivative.hh"

void cleanup_dispatch(Kernel& kernel, Ex& tr, Ex::iterator& it)
	{
	//std::cerr << "cleanup at " << *it->name << std::endl;
	if(it->is_zero())  {
		::zero(it->multiplier);
		tr.erase_children(it);
		it->name=name_set.insert("1").first;
		}
	else if(*it->name=="\\prod")       cleanup_productlike(kernel, tr, it);
	else if(*it->name=="\\sum")        cleanup_sumlike(kernel, tr, it);
	else if(*it->name=="\\expression") cleanup_expressionlike(kernel, tr, it);
	else if(*it->name=="\\components") cleanup_components(kernel, tr, it);

	const Derivative *der = kernel.properties.get<Derivative>(it);
	if(der) cleanup_derivative(kernel, tr, it);
	}

void cleanup_productlike(Kernel& k, Ex&tr, Ex::iterator& it)
	{
	assert(*it->name=="\\prod");

	// Flatten prod children inside this prod node.
	auto sib=tr.begin(it);
	while(sib!=tr.end(it)) {
		if(*sib->name=="\\prod") {
			multiply(it->multiplier, *sib->multiplier);
			tr.flatten(sib); 
			sib=tr.erase(sib);
			}
		else ++sib;
		}

	if(tr.number_of_children(it)==1)
		if(tr.begin(it)->is_range_wildcard())
			return;

	// Collect all multipliers and remove resulting '1' nodes.
	auto facs=tr.begin(it);
	multiplier_t factor=1;
	while(facs!=tr.end(it)) {
		factor*=*facs->multiplier;
		if(facs->is_rational()) {
			multiplier_t tmp; // FIXME: there is a bug in gmp which means we have to put init on next line.
			tmp=(*facs->name).c_str();
			factor*=tmp;
		   facs=tr.erase(facs);
			if(facs==tr.end())
				facs=tr.end(it);
			}
		else {
			one(facs->multiplier);
			++facs;
			}
		}
	multiply(it->multiplier,factor);

	// Handle edge cases where the product should collapse to a single node,
	// e.g. when we have just a single factor, or when the product vanishes.

	if(tr.number_of_children(it)==1) { // i.e. from '3*4*7*a*9'
		tr.begin(it)->fl.bracket=it->fl.bracket;
		tr.begin(it)->fl.parent_rel=it->fl.parent_rel;
		tr.begin(it)->multiplier=it->multiplier;
		tr.flatten(it);
		it=tr.erase(it);
		push_down_multiplier(k, tr, it);
		}
	else if(tr.number_of_children(it)==0) { // i.e. from '3*4*7*9' 
		it->name=name_set.insert("1").first;
		}

	if(it->is_zero()) {
		::zero(it->multiplier);
		tr.erase_children(it);
		it->name=name_set.insert("1").first;
		}

	}

void cleanup_sumlike(Kernel& k, Ex&tr, Ex::iterator& it)
	{
	assert(*it->name=="\\sum");

	if(tr.number_of_children(it)==1)
		if(tr.begin(it)->is_range_wildcard())
			return;

	// Flatten sums which are supposed to be flat.
	// FIXME: this does too much and we should not be using Algorithm
	// objects here anyway.
//	std::cerr << *it->name << std::endl;
	flatten_sum fs(k,tr);
	if(fs.can_apply(it)) {
		fs.apply(it);
		}
	// Remove children which are 0
	Ex::sibling_iterator sib=tr.begin(it);
	while(sib!=tr.end(it)) {
		if(sib->is_zero())
			sib=tr.erase(sib);
		else
			++sib;
		}


//	std::cerr << *it->name << std::endl;
//	tr.print_recursive_treeform(std::cerr, tr.begin());
   // Collect all equal terms.
	collect_terms ct(k, tr);
	if(ct.can_apply(it))
		ct.apply(it);


	push_down_multiplier(k, tr, it);
	}

void push_down_multiplier(Kernel& k, Ex& tr, Ex::iterator it)
	{
	auto mult=*it->multiplier;
	if(mult==1) return;

	if(*it->name=="\\sum") {
		auto sib=tr.begin(it);
		while(sib!=tr.end(it)) {
			multiply(sib->multiplier, mult);
			push_down_multiplier(k, tr, sib);
			++sib;
			}
		one(it->multiplier);
		}
	else if(*it->name=="\\components") {
		Ex::sibling_iterator sib=tr.end(it);
		--sib;
		// Examine all index value sets and push the multiplier
		// in there.
		cadabra::do_list(tr, sib, [&](Ex::iterator nd) {
				Ex::sibling_iterator val=tr.begin(nd);
				++val;
				multiply(val->multiplier, mult);
				push_down_multiplier(k, tr, val);
				return true;
				});
		
		one(it->multiplier);
		}
	}

void cleanup_components(Kernel& k, Ex&tr, Ex::iterator& it)
	{
	assert(*it->name=="\\components");

	push_down_multiplier(k, tr, it);

	// If this component node has no free indices, get rid of all
	// the baggage and turn into a normal expression.

	//std::cerr << "components cleanup: " << Ex(it) << std::endl;

	auto comma=tr.begin(it);
	if(*comma->name=="\\comma") {
		std::cerr << "components node for a scalar" << std::endl;
		tr.flatten(it); // unwrap comma
		it=tr.erase(it);
//		tr.flatten(it); // unwrap equals
//		it=tr.erase(it);
//		it=tr.erase(it); // remove empty comma for index values
		}
	}

void cleanup_expressionlike(Kernel& k, Ex&tr, Ex::iterator& it)
	{
	assert(*it->name=="\\expression");

	// Can only have one child which contains actual expression data;
	// all others are meta-data like \asymimplicit. If this child has
	// zero multiplier, simplify.

	Ex::sibling_iterator sib=tr.begin(it);
	if(sib==tr.end(it)) return;
	if(sib->is_zero()) {
		// FIXME: duplicate of node_zero in Algorithm.
		tr.erase_children(sib);
		sib->name=name_set.insert("1").first;
		}
	}

void cleanup_derivative(Kernel& k, Ex& tr, Ex::iterator& it)
	{
	// Nested derivatives with the same name should be flattened, but
	// only if both the outer derivative and the inner derivative have
	// an index (otherwise D(D(A)) becomes D(A) which is wrong).

	// Find first non-index child.

	Ex::sibling_iterator sib=tr.begin(it);
	if(sib==tr.end(it)) return;

	while(sib->is_index()) {
		++sib;
		if(sib==tr.end(it))
			throw ConsistencyException("Encountered Derivative object without argument on which to act.");
		// FIXME: the above is not correct when a derivative is declared without argument,
		// like in \Omega::Derivative; A::Depends(\Omega).
		}

	// FIXME: this ignores that derivatives can have functional child
	// nodes which are interpreted as 'object wrt. with derivative should be taken'.

	if(it->name == sib->name) {
		if(Algorithm::number_of_direct_indices(it)>0 && Algorithm::number_of_direct_indices(sib)>0) {
			multiply(it->multiplier, *sib->multiplier);
			tr.flatten(sib);
			tr.erase(sib);
			}
		}
	}

void cleanup_dispatch_deep(Kernel& k, Ex& tr, dispatcher_t dispatch)
	{
	// Cleanup the entire tree starting from the deepest nodes and
	// working upwards. 

	// This duplicates work of Algorithm::apply, but we want to have an
	// independent cleanup unit which does not rely on things we may
	// want to change in Algorithm::apply in the future, and we do not
	// want to make recursive calls into that function either. And it is
	// simple enough anyway.

	Ex::post_order_iterator it=tr.begin();
	it.descend_all();
	while(it!=tr.end()) {
		Ex::post_order_iterator next=it;
		++next;
		Ex::iterator tmp=it;
		dispatch(k, tr, tmp);
		it=next;
		}
	}

