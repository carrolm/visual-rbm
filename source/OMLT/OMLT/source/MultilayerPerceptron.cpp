// std
#include <assert.h>

 // windows
#include <malloc.h>
#include <intrin.h>

// extern
#include <cJSON.h>

// OMLT
#include "MultilayerPerceptron.h"
#include "Common.h"

namespace OMLT
{
	extern __m128 _mm_rectifiedlinear_ps(__m128 x0);
	extern __m128 _mm_sigmoid_ps(__m128 x0);

	MultilayerPerceptron::~MultilayerPerceptron()
	{
		for(uint32_t k = 0; k < _layers.size(); k++)
		{
			delete _layers[k];
		}

		// offset index because first ptr is nullptr
		for(uint32_t k = 1; k < _accumulations.size(); k++)
		{
			_aligned_free(_accumulations[k]);
		}

		for(uint32_t k = 0; k < _activations.size(); k++)
		{
			_aligned_free(_activations[k]);
		}
	}

	void MultilayerPerceptron::FeedForward(float* input_vector, float* output_vector)
	{
		FeedForward(input_vector, output_vector, _layers.size() - 1);
	}

	void MultilayerPerceptron::FeedForward( float* input_vector, float* output_vector, uint32_t layer_index)
	{
		assert(layer_index < _layers.size());

		memcpy(_activations[0], input_vector, sizeof(float) * _layers.front()->inputs);

		for(uint32_t L = 0; L <= layer_index; L++)
		{
			const Layer& layer = *_layers[L];

			float* input_activation_head = _activations[L];
			float* output_accumulation_head = _accumulations[L+1];
			float* output_activation_head = _activations[L+1];

			const uint32_t input_blocks = BlockCount(layer.inputs);
			for(uint32_t j = 0; j < layer.outputs; j++)
			{
				float* input_activation = input_activation_head;
				float* weight_vector = layer.weights[j];

				__m128 out_j = _mm_setzero_ps();
				for(uint32_t i = 0; i < input_blocks; i++)
				{
					__m128 input = _mm_load_ps(input_activation);
					__m128 w = _mm_load_ps(weight_vector);

					// dot product
					out_j = _mm_add_ps(out_j, _mm_mul_ps(input, w));

					input_activation += 4;
					weight_vector += 4;
				}

				// finish up our dot product
				out_j = _mm_hadd_ps(out_j, out_j);
				out_j = _mm_hadd_ps(out_j, out_j);

				// bring it back
				_mm_store_ss(output_accumulation_head + j, out_j);

			}

			// add in the biases and calc activation
			const uint32_t output_blocks = BlockCount(layer.outputs);

			float* output_activation = output_activation_head;
			float* output_accumulation = output_accumulation_head;
			float* biases = layer.biases;

			// now calculate activation on all these buggers
			
			for(uint32_t j = 0; j < output_blocks; j++)
			{
				__m128 b = _mm_load_ps(biases);
				// load our current accumulation
				__m128 acc = _mm_load_ps(output_accumulation);
				// add in the bias
				acc = _mm_add_ps(acc, b);

				__m128 act;
				switch(layer.function)
				{
				case ActivationFunction::Linear:
					act = acc;
					break;
				case ActivationFunction::RectifiedLinear:
					act = _mm_rectifiedlinear_ps(acc);
					break;
				case ActivationFunction::Sigmoid:
					act = _mm_sigmoid_ps(acc);
					break;
				}

				_mm_store_ps(output_activation, act);

				// move up our pointers
				output_accumulation += 4;
				output_activation += 4;
				biases += 4;
			}

			// move this back to beginning of buffer
			output_activation = output_activation_head;
			// zero out the dangling bits here (left over memory from 16 byte padding)
			// so we don't screw up next layer's dot products
			for(uint32_t j = layer.outputs; j < output_blocks * 4; j++)
			{
				output_activation[j] = 0.0f;
			}
		}

		// memcpy activation to the output buffer
		memcpy(output_vector, _activations[layer_index + 1], sizeof(float) * _layers[layer_index]->outputs);
	}

	MultilayerPerceptron::Layer* MultilayerPerceptron::GetLayer( uint32_t index )
	{
		assert(index < _layers.size());
		return _layers[index];
	}

	MultilayerPerceptron::Layer* MultilayerPerceptron::PopInputLayer()
	{
		assert(_layers.size() > 0);
		
		// first accumulation ptr is nullptr
		_accumulations.erase(_accumulations.begin());
		if(_layers.size() > 1)
		{
			_aligned_free(_accumulations.front());
			_accumulations.front() = nullptr;

		}		

		_aligned_free(_activations.front());
		_activations.erase(_activations.begin());

		MLP::Layer* result = _layers.front();
		_layers.erase(_layers.begin());

		return result;
	}

	MultilayerPerceptron::Layer* MultilayerPerceptron::PopOutputLayer()
	{
		assert(_layers.size() > 0);

		if(_layers.size() > 1)
		{
			_aligned_free(_accumulations.back());
		}
		_accumulations.erase(_accumulations.end() - 1);
		
		_aligned_free(_activations.back());
		_activations.erase(_activations.end() - 1);

		MLP::Layer* result = _layers.back();
		_layers.erase(_layers.end() - 1);

		return result;
	}

	bool MultilayerPerceptron::AddLayer( Layer* in_layer)
	{
		if(_layers.size() == 0)
		{
			// we don't get the accumulations for input
			_accumulations.push_back(nullptr);
			
			// we'll always copy in input vectors to the first slot, so give us some aligned memory here
			size_t input_alloc_size = sizeof(float) * BlockCount(in_layer->inputs) * 4;
			_activations.push_back((float*)_aligned_malloc(input_alloc_size, 16));

			memset(_activations.front(), 0x00, input_alloc_size);
		}
		else if(_layers.back()->outputs != in_layer->inputs)
		{
			// unit count mismatch between existing top layer and new top layer
			return false;
		}

		_layers.push_back(in_layer);
		size_t alloc_size = sizeof(float) * (in_layer->outputs + 4);
		_accumulations.push_back((float*)_aligned_malloc(alloc_size, 16));
		_activations.push_back((float*)_aligned_malloc(alloc_size, 16));
		
		memset(_accumulations.back(), 0x00, alloc_size);
		memset(_activations.back(), 0x00, alloc_size);
		
		return true;
	}

	std::string MultilayerPerceptron::ToJSON() const
	{
		cJSON* root = cJSON_CreateObject();
		cJSON* root_layer = cJSON_CreateArray();
		cJSON_AddStringToObject(root, "Type", "MultilayerPerceptron");
		cJSON_AddItemToObject(root, "Layers", root_layer);

		for(auto it = _layers.begin(); it < _layers.end(); ++it)
		{
			cJSON* layer = cJSON_CreateObject();
			cJSON_AddItemToArray(root_layer, layer);

			cJSON_AddNumberToObject(layer, "Inputs", (*it)->inputs);			
			cJSON_AddNumberToObject(layer, "Outputs", (*it)->outputs);
			cJSON_AddStringToObject(layer, "Function", ActivationFunctionNames[(*it)->function]);
			cJSON_AddItemToObject(layer, "Biases", cJSON_CreateFloatArray((*it)->biases, (*it)->outputs));
			
			cJSON* layer_weights = cJSON_CreateArray();
			cJSON_AddItemToObject(layer, "Weights", layer_weights);

			// now serialize out all those weights
			for(uint32_t j = 0; j < (*it)->outputs; j++)
			{
				cJSON_AddItemToArray(layer_weights, cJSON_CreateFloatArray((*it)->weights[j], (*it)->inputs));
			}
		}

		char* json_buffer = cJSON_Print(root);
		std::string result(json_buffer);

		// cleanup
		free(json_buffer);
		cJSON_Delete(root);

		return result;
	}

	MultilayerPerceptron* MultilayerPerceptron::FromJSON( cJSON* in_root)
	{
		if(in_root)
		{
			MultilayerPerceptron* mlp = new MultilayerPerceptron();
			cJSON* type =  cJSON_GetObjectItem(in_root, "Type");
			if(strcmp(type->valuestring, "MultilayerPerceptron") != 0)
			{
				goto Malformed;
			}
			cJSON* layers = cJSON_GetObjectItem(in_root, "Layers");
			if(layers == nullptr)
			{
				goto Malformed;
			}

			const int layer_count = cJSON_GetArraySize(layers);
			for(int k = 0; k < layer_count; k++)
			{
				cJSON* layers_k = cJSON_GetArrayItem(layers, k);
				
				cJSON* inputs = cJSON_GetObjectItem(layers_k, "Inputs");
				cJSON* outputs = cJSON_GetObjectItem(layers_k, "Outputs");
				cJSON* function = cJSON_GetObjectItem(layers_k, "Function");
				cJSON* biases = cJSON_GetObjectItem(layers_k, "Biases");
				cJSON* weights = cJSON_GetObjectItem(layers_k, "Weights");

				// make sure we didn't get any nulls back
				if(inputs && outputs && function && biases && weights)
				{
					ActivationFunction_t n_function = (ActivationFunction_t)-1;
					for(int func = 0; func < ActivationFunction::Count; func++)
					{
						if(strcmp(function->valuestring, ActivationFunctionNames[func]) == 0)
						{
							n_function = (ActivationFunction_t)func;
							break;
						}
					}

					if(n_function == -1)
					{
						goto Malformed;
					}

					if(cJSON_GetArraySize(biases) == outputs->valueint &&
						cJSON_GetArraySize(weights) == outputs->valueint)
					{

						Layer* n_layer = new Layer(inputs->valueint, outputs->valueint, n_function);
						mlp->AddLayer(n_layer);

						cJSON* biases_it = cJSON_CreateArrayIterator(biases);
						cJSON* weights_it = cJSON_CreateArrayIterator(weights);

						for(uint32_t j = 0; j < n_layer->outputs; j++)
						{
							// set this bias
							cJSON_ArrayIteratorMoveNext(biases_it);
							n_layer->biases[j] = (float)cJSON_ArrayIteratorCurrent(biases_it)->valuedouble;
							// and the weight vectors
							cJSON_ArrayIteratorMoveNext(weights_it);
							cJSON* w_j = cJSON_ArrayIteratorCurrent(weights_it);
							if(cJSON_GetArraySize(w_j) == n_layer->inputs)
							{
								cJSON* w_j_it = cJSON_CreateArrayIterator(w_j);
								for(uint32_t i = 0; i < n_layer->inputs; i++)
								{
									cJSON_ArrayIteratorMoveNext(w_j_it);
									n_layer->weights[j][i] = (float)cJSON_ArrayIteratorCurrent(w_j_it)->valuedouble;
								}
								cJSON_Delete(w_j_it);
							}
							else
							{
								cJSON_Delete(biases_it);
								cJSON_Delete(biases_it);
								cJSON_Delete(weights_it);
								goto Malformed;
							}
						}
						cJSON_Delete(biases_it);
						cJSON_Delete(weights_it);
					}
				}
				else
				{
					goto Malformed;
				}
			}

			return mlp;
Malformed:
			delete mlp;
			return nullptr;
		}

		return nullptr;
	}

	MultilayerPerceptron* MultilayerPerceptron::FromJSON(const std::string& in_json)
	{
		cJSON* root = cJSON_Parse(in_json.c_str());

		MLP* mlp = FromJSON(root);
		cJSON_Delete(root);

		return mlp;		
	}

	MultilayerPerceptron::Layer::Layer( uint32_t in_inputs, uint32_t in_outputs, ActivationFunction_t in_function )
		: inputs(in_inputs)
		, outputs(in_outputs)
		, function(in_function)
		, biases(nullptr)
		, weights(nullptr)
	{

		// allocate space for biases
		const uint32_t biases_alloc_size = sizeof(float) * BlockCount(outputs) * 4;
		biases = (float*)_aligned_malloc(biases_alloc_size, 16);
		memset(biases, 0x00, biases_alloc_size);

		
		// allocate space for weights
		weights = new float*[outputs];

		const uint32_t weight_alloc_size = sizeof(float) * BlockCount(inputs) * 4;
		for(uint32_t j = 0; j < outputs; j++)
		{
			weights[j] = (float*)_aligned_malloc(weight_alloc_size, 16);
			memset(weights[j], 0x00, weight_alloc_size);
		}
	}

	MultilayerPerceptron::Layer::~Layer()
	{
		_aligned_free(biases);

		for(uint32_t j = 0; j < outputs; j++)
		{
			_aligned_free(weights[j]);
		}
		delete[] weights;
	}
}